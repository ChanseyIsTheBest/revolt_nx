/* rv_events.c -- see rv_events.h. MIT licensed. */

#include <string.h>

#include "rv_events.h"
#include "rv_cursor.h"
#include "rv_log.h"

#define RVE_QUEUE 512

typedef struct {
    SDL_Event event;
    uint8_t isKey;      /* paced */
} RvPending;

static RvPending g_queue[RVE_QUEUE];
static int g_head, g_count;

/* Our view of the keyboard, overlaid on SDL's. Only scancodes we synthesise
 * are ever set; everything else passes through unchanged. */
static Uint8 g_overlay[SDL_NUM_SCANCODES];
static Uint8 g_merged[SDL_NUM_SCANCODES];
static int g_overlayActive;

/* One key event per frame.
 *
 * "A frame" was originally taken to mean "SDL_PollEvent returned 0", which is
 * true only while the queue actually empties. RVGL polls game controllers, and
 * a resting stick still produces axis events every frame -- so with a pad
 * connected the drain may never hit zero and the pacing would starve: the
 * keyboard result would queue up and never be delivered.
 *
 * A clock is used as well. Whichever comes first releases the next key, so
 * pacing survives a continuous event stream. */
static int g_keySentThisFrame;
static Uint32 g_lastKeyAt;

#define RVE_KEY_INTERVAL_MS 16

static void Enqueue(const SDL_Event *event, int isKey) {
    if (g_count >= RVE_QUEUE) return;   /* drop rather than overwrite */
    const int slot = (g_head + g_count) % RVE_QUEUE;
    g_queue[slot].event = *event;
    g_queue[slot].isKey = (uint8_t)isKey;
    ++g_count;
}

void rv_events_queue_key(SDL_Keycode key, SDL_Scancode scancode) {
    SDL_Event event;

    memset(&event, 0, sizeof event);
    event.type = SDL_KEYDOWN;
    event.key.state = SDL_PRESSED;
    event.key.keysym.sym = key;
    event.key.keysym.scancode = scancode;
    event.key.keysym.mod = KMOD_NONE;
    Enqueue(&event, 1);

    event.type = SDL_KEYUP;
    event.key.state = SDL_RELEASED;
    Enqueue(&event, 1);
}

void rv_events_queue_text(const char *text) {
    if (text == NULL) return;

    while (*text != '\0') {
        SDL_Event event;
        memset(&event, 0, sizeof event);
        event.type = SDL_TEXTINPUT;

        size_t n = 0;
        while (text[n] != '\0' && n < sizeof event.text.text - 1) ++n;

        /* Never split a codepoint: the applet returns UTF-8 and a cut mid
         * sequence delivers two invalid fragments. */
        if (text[n] != '\0')
            while (n > 0 && (text[n] & 0xC0) == 0x80) --n;
        if (n == 0) break;

        memcpy(event.text.text, text, n);
        event.text.text[n] = '\0';
        Enqueue(&event, 0);

        text += n;
    }
}

void rv_events_queue_raw(const SDL_Event *event) {
    if (event != NULL) Enqueue(event, 0);
}

int rv_events_pending(void) { return g_count; }

static void ApplyToOverlay(const SDL_Event *event) {
    const SDL_Scancode scancode = event->key.keysym.scancode;
    if (scancode <= 0 || scancode >= SDL_NUM_SCANCODES) return;

    const Uint8 down = (event->type == SDL_KEYDOWN) ? 1 : 0;
    g_overlay[scancode] = down;
    g_overlayActive = 1;

    /* Also write it straight into the buffer rv_get_keyboard_state hands out.
     *
     * SDL's contract says that pointer stays valid for the process lifetime,
     * so a caller is entitled to fetch it once and keep reading it -- and a
     * caller that does would never see an overlay applied only at call time.
     * Writing here means a cached pointer works too. */
    g_merged[scancode] = down;
}

int rv_poll_event(SDL_Event *event) {
    if (event == NULL) return 0;

    /* Synthetic first, so a clear always lands before the text that replaces
     * it -- ordering the engine would otherwise see reversed. */
    while (g_count > 0) {
        const RvPending *next = &g_queue[g_head];

        /* One key event per frame; text is unrestricted. */
        if (next->isKey && g_keySentThisFrame &&
            SDL_GetTicks() - g_lastKeyAt < RVE_KEY_INTERVAL_MS)
            break;

        *event = next->event;
        const int wasKey = next->isKey;

        /* Stamp on the way out rather than at queue time: the focused window
         * can change while events are waiting. */
        SDL_Window *window = SDL_GetKeyboardFocus();
        const Uint32 windowID = window != NULL ? SDL_GetWindowID(window) : 0;
        const Uint32 now = SDL_GetTicks();

        if (event->type == SDL_TEXTINPUT) {
            event->text.windowID = windowID;
            event->text.timestamp = now;
        } else if (event->type == SDL_FINGERDOWN ||
                   event->type == SDL_FINGERUP ||
                   event->type == SDL_FINGERMOTION) {
            event->tfinger.timestamp = now;
        } else {
            event->key.windowID = windowID;
            event->key.timestamp = now;
            ApplyToOverlay(event);
        }

        g_head = (g_head + 1) % RVE_QUEUE;
        --g_count;
        if (wasKey) {
            g_keySentThisFrame = 1;
            g_lastKeyAt = SDL_GetTicks();
        }

        rv_input_observe_event(event);
        return 1;
    }

    for (;;) {
        const int result = SDL_PollEvent(event);
        if (result == 0) break;

        /* A is the tap while the cursor is up; it must not also reach the
         * engine as a button press. */
        if (rv_cursor_swallows(event)) continue;

        rv_input_observe_event(event);
        return result;
    }

    /* Drain finished: the engine has consumed everything for this frame. */
    g_keySentThisFrame = 0;
    return 0;
}

const Uint8 *rv_get_keyboard_state(int *numkeys) {
    int count = 0;
    const Uint8 *real = SDL_GetKeyboardState(&count);

    if (!g_overlayActive) {
        if (numkeys != NULL) *numkeys = count;
        return real;
    }

    /* Merge rather than replace. An engine that edge-detects backspace by
     * polling this -- which is the reading that best explains RVGL ignoring
     * 128 pushed KEYDOWNs -- needs to see the key held, while every real key
     * must still read correctly. */
    if (count > SDL_NUM_SCANCODES) count = SDL_NUM_SCANCODES;
    if (real != NULL) memcpy(g_merged, real, (size_t)count);

    for (int i = 0; i < count; ++i)
        if (g_overlay[i]) g_merged[i] = 1;

    if (numkeys != NULL) *numkeys = count;
    return g_merged;
}
