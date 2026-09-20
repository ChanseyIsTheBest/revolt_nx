/* rv_cursor.c -- see rv_cursor.h. MIT licensed. */

#include <math.h>
#include <string.h>

#include "rv_cursor.h"
#include "rv_events.h"
#include "rv_log.h"
#include "nx_pointer.h"

#define RV_TRIGGER_ON      12000    /* of 32767; a button-backed trigger is 0 or max */
#define RV_STICK_DEADZONE   7000
#define RV_CURSOR_SPEED      900.0f /* pixels per second at full deflection */

/* Well clear of any finger the console could report, so a real touch and a
 * synthetic tap can never be confused for one another. */
#define RV_CURSOR_FINGER   0x52564358

static SDL_GameController *g_pad;
static int g_visible;
static int g_screenW = 1920, g_screenH = 1080;
static Uint32 g_lastUpdate;

static int g_chordHeld;     /* ZL+ZR edge detection */
static int g_tapHeld;       /* A edge detection */

void rv_cursor_init(const char *dataDir) {
    nxp_init(g_screenW, g_screenH, dataDir);
    rvnx_log_print(4, "cursor", "ZL+ZR toggles the pointer, left stick moves, A taps");
}

int rv_cursor_visible(void) { return g_visible; }

static SDL_GameController *Pad(void) {
    if (g_pad != NULL && SDL_GameControllerGetAttached(g_pad)) return g_pad;

    g_pad = NULL;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (!SDL_IsGameController(i)) continue;
        g_pad = SDL_GameControllerOpen(i);
        if (g_pad != NULL) break;
    }
    return g_pad;
}

/* Normalised coordinates, which is what SDL_TouchFingerEvent carries. */
static void EmitFinger(Uint32 type, float x, float y) {
    SDL_Event event;
    memset(&event, 0, sizeof event);

    event.type = type;
    event.tfinger.touchId = 0;
    event.tfinger.fingerId = RV_CURSOR_FINGER;
    event.tfinger.x = x / (float)g_screenW;
    event.tfinger.y = y / (float)g_screenH;
    event.tfinger.pressure = (type == SDL_FINGERUP) ? 0.0f : 1.0f;

    rv_events_queue_raw(&event);
}

void rv_cursor_update(void) {
    SDL_GameController *pad = Pad();
    if (pad == NULL) return;

    const Uint32 now = SDL_GetTicks();
    const Uint32 elapsed = (g_lastUpdate == 0) ? 16 : now - g_lastUpdate;
    g_lastUpdate = now;

    /* --- ZL+ZR toggles, on the press rather than while held --- */
    const int zl = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT)
                       > RV_TRIGGER_ON;
    const int zr = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT)
                       > RV_TRIGGER_ON;
    const int chord = zl && zr;

    if (chord && !g_chordHeld) {
        g_visible = !g_visible;
        nxp_set_visible(g_visible);
        rvnx_log_print(4, "cursor", "%s", g_visible ? "on" : "off");

        /* Releasing a held tap on the way out would land somewhere arbitrary. */
        if (!g_visible && g_tapHeld) {
            float x, y;
            nxp_pos(&x, &y);
            EmitFinger(SDL_FINGERUP, x, y);
            g_tapHeld = 0;
        }
    }
    g_chordHeld = chord;

    if (!g_visible) return;

    /* --- left stick moves it --- */
    int dx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
    int dy = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
    if (abs(dx) < RV_STICK_DEADZONE) dx = 0;
    if (abs(dy) < RV_STICK_DEADZONE) dy = 0;

    if (dx != 0 || dy != 0) {
        const float seconds = (float)elapsed / 1000.0f;
        const float scale = RV_CURSOR_SPEED * seconds / 32767.0f;
        nxp_move((float)dx * scale, (float)dy * scale);

        if (g_tapHeld) {
            /* Dragging: the engine needs the motion, not just the endpoints. */
            float x, y;
            nxp_pos(&x, &y);
            EmitFinger(SDL_FINGERMOTION, x, y);
        }
    }

    /* --- A taps --- */
    const int a = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A);
    if (a != g_tapHeld) {
        float x, y;
        nxp_pos(&x, &y);
        EmitFinger(a ? SDL_FINGERDOWN : SDL_FINGERUP, x, y);
        g_tapHeld = a;
    }
}

/* While the cursor is up the pad belongs to it, so the engine is shown a pad
 * at rest. Returning the real values would have the stick steering the menu
 * under the cursor at the same time as moving the cursor over it. */

int rv_pad_get_button(SDL_GameController *pad, SDL_GameControllerButton b) {
    if (g_visible) return 0;
    return SDL_GameControllerGetButton(pad, b);
}

Sint16 rv_pad_get_axis(SDL_GameController *pad, SDL_GameControllerAxis a) {
    if (g_visible) return 0;
    return SDL_GameControllerGetAxis(pad, a);
}

Sint16 rv_joy_get_axis(SDL_Joystick *joystick, int axis) {
    if (g_visible) return 0;
    return SDL_JoystickGetAxis(joystick, axis);
}

Uint8 rv_joy_get_button(SDL_Joystick *joystick, int button) {
    if (g_visible) return 0;
    return SDL_JoystickGetButton(joystick, button);
}

Uint8 rv_joy_get_hat(SDL_Joystick *joystick, int hat) {
    if (g_visible) return SDL_HAT_CENTERED;
    return SDL_JoystickGetHat(joystick, hat);
}

int rv_cursor_swallows(const SDL_Event *event) {
    if (!g_visible || event == NULL) return 0;

    /* Every pad event, not just A. The polling wrappers above already report
     * a pad at rest; letting the events through would contradict that, and an
     * engine that uses both would see the stick centred and still get a motion
     * event for it. */
    switch (event->type) {
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP:
        case SDL_CONTROLLERAXISMOTION:
        case SDL_JOYBUTTONDOWN:
        case SDL_JOYBUTTONUP:
        case SDL_JOYAXISMOTION:
        case SDL_JOYHATMOTION:
        case SDL_JOYBALLMOTION:
            return 1;
        default:
            return 0;
    }
}

void rv_gl_swap_window(SDL_Window *window) {
    int w = 0, h = 0;
    SDL_GL_GetDrawableSize(window, &w, &h);
    if (w > 0 && h > 0 && (w != g_screenW || h != g_screenH)) {
        g_screenW = w;
        g_screenH = h;
        nxp_set_screen(w, h);
    }

    rv_cursor_update();

    /* After the game has drawn, before the swap -- the only point where the
     * overlay lands on top of the finished frame. */
    if (g_visible) nxp_draw();

    SDL_GL_SwapWindow(window);
}
