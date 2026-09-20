/* rv_input.c -- see rv_input.h. MIT licensed. */

#include <stdarg.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

#include <SDL2/SDL.h>

#include "config.h"
#include "rv_input.h"
#include "rv_log.h"
#include "libc_shim.h"
#include "rv_events.h"
#include "rv_paths.h"

/* Button and axis order reported by devkitPro's switch-sdl2 joystick driver:
 *
 *   buttons  0 A   1 B   2 X   3 Y   4 LStick  5 RStick  6 L   7 R
 *            8 ZL  9 ZR  10 Plus  11 Minus
 *            12 Left  13 Up  14 Right  15 Down
 *   axes     0 leftX  1 leftY  2 rightX  3 rightY
 *
 * Taken from a shipped Switch port (DKC1Recomp's switch_gamepad.c) rather than
 * the SDL source, because this is the layout the portlibs build produces.
 *
 * The COUNTS are not assumed. devkitPro has changed this driver across SDL
 * releases: some builds expose ZL/ZR as analogue axes 4 and 5, some expose the
 * D-pad as a hat rather than four buttons. Writing `lefttrigger:b8` against a
 * six-axis build gives a trigger that never fires; `dpup:b13` against a hat
 * build gives no D-pad at all. Both are silent. So the mapping is built from
 * what SDL reports for the pad actually present, and the branch it took is
 * logged.
 */
enum {
    kBtnA = 0, kBtnB = 1, kBtnX = 2, kBtnY = 3,
    kBtnLStick = 4, kBtnRStick = 5,
    kBtnL = 6, kBtnR = 7,
    kBtnZL = 8, kBtnZR = 9,
    kBtnPlus = 10, kBtnMinus = 11,
    kBtnLeft = 12, kBtnUp = 13, kBtnRight = 14, kBtnDown = 15,
};

#ifndef RVNX_POSITIONAL_FACE_BUTTONS
/* 0 = Nintendo labels (SDL A is the Switch's A, the right button).
 * 1 = positional (SDL A is the bottom button, the Switch's B). */
#define RVNX_POSITIONAL_FACE_BUTTONS 0
#endif

static int Append(char **cursor, size_t *left, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static int Append(char **cursor, size_t *left, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(*cursor, *left, fmt, args);
    va_end(args);

    if (n <= 0 || (size_t)n >= *left) return 0;
    *cursor += n;
    *left -= (size_t)n;
    return 1;
}

size_t rv_input_build_mapping(const char *guid, const char *name,
                              const char *platform, int nintendoFaceOrder,
                              int numAxes, int numButtons, int numHats,
                              RvPadShape *shape, char *out, size_t capacity) {
    if (guid == NULL || out == NULL || capacity == 0) return 0;

    RvPadShape local;
    if (shape == NULL) shape = &local;
    memset(shape, 0, sizeof *shape);

    /* Below this there is nothing useful to map, and guessing would be worse
     * than leaving the pad raw. */
    if (numAxes < 2 || numButtons < 4) return 0;

    char *cursor = out;
    size_t left = capacity;

    /* The platform string comes from SDL at runtime, not from a constant.
     *
     * devkitPro's SDL 2.28.5 reports "Unknown (see SDL_platform.h)", NOT
     * "Switch" -- confirmed from a hardware log. SDL discards any mapping
     * whose platform field does not match what SDL_GetPlatform() returns, so a
     * hardcoded "platform:Switch" here is a line SDL will never apply. */
    if (!Append(&cursor, &left, "%s,%s,platform:%s,", guid,
                (name != NULL && *name != '\0') ? name
                                                : "Nintendo Switch Controller",
                (platform != NULL && *platform != '\0') ? platform : "Switch"))
        return 0;

    const int a = nintendoFaceOrder ? kBtnA : kBtnB;
    const int b = nintendoFaceOrder ? kBtnB : kBtnA;
    const int x = nintendoFaceOrder ? kBtnX : kBtnY;
    const int y = nintendoFaceOrder ? kBtnY : kBtnX;

    if (!Append(&cursor, &left, "a:b%d,b:b%d,", a, b)) return 0;
    if (numButtons > kBtnY && !Append(&cursor, &left, "x:b%d,y:b%d,", x, y)) return 0;

    if (!Append(&cursor, &left, "leftx:a0,lefty:a1,")) return 0;
    if (numAxes >= 4) {
        if (!Append(&cursor, &left, "rightx:a2,righty:a3,")) return 0;
        shape->rightStick = 1;
    }

    if (numButtons > kBtnRStick &&
        !Append(&cursor, &left, "leftstick:b%d,rightstick:b%d,",
                kBtnLStick, kBtnRStick)) return 0;

    if (numButtons > kBtnR &&
        !Append(&cursor, &left, "leftshoulder:b%d,rightshoulder:b%d,",
                kBtnL, kBtnR)) return 0;

    /* Triggers: analogue when the driver exposes them that way, digital
     * otherwise. RVGL reads both with SDL_GameControllerGetAxis -- a
     * button-backed trigger reports 0 or 32767. */
    if (numAxes >= 6) {
        if (!Append(&cursor, &left, "lefttrigger:a4,righttrigger:a5,")) return 0;
        shape->analogueTriggers = 1;
    } else if (numButtons > kBtnZR) {
        if (!Append(&cursor, &left, "lefttrigger:b%d,righttrigger:b%d,",
                    kBtnZL, kBtnZR)) return 0;
    }

    if (numButtons > kBtnMinus &&
        !Append(&cursor, &left, "start:b%d,back:b%d,", kBtnPlus, kBtnMinus))
        return 0;

    if (numHats >= 1) {
        if (!Append(&cursor, &left,
                    "dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,")) return 0;
        shape->hatDpad = 1;
    } else if (numButtons > kBtnDown) {
        if (!Append(&cursor, &left, "dpleft:b%d,dpup:b%d,dpright:b%d,dpdown:b%d,",
                    kBtnLeft, kBtnUp, kBtnRight, kBtnDown)) return 0;
        shape->buttonDpad = 1;
    }

    /* Trim the trailing comma. SDL tolerates it, but this line can end up in a
     * file the user edits. */
    if (cursor > out && cursor[-1] == ',') { --cursor; *cursor = '\0'; }

    shape->numAxes = numAxes;
    shape->numButtons = numButtons;
    shape->numHats = numHats;
    return (size_t)(cursor - out);
}

/* ------------------------------------------------------------------ */

static int LoadUserOverrides(void) {
    const char *path = rv_paths()->userDb;

    SDL_RWops *file = SDL_RWFromFile(path, "rb");
    if (file == NULL) return 0;

    const int added = SDL_GameControllerAddMappingsFromRW(file, 1);
    if (added > 0)
        rvnx_log_print(4, "input", "loaded %d user mapping(s) from %s", added, path);
    return added > 0 ? added : 0;
}

/* RVGL loads profiles/gamecontrollerdb.txt itself, on top of the databases it
 * ships -- found in the binary's string table. Seeding our mapping there means
 * it survives anything that resets SDL's in-memory list, and leaves the user a
 * file to edit in the place RVGL's own docs point at.
 *
 * Written only when absent, so an edited file is never clobbered. */
static void SeedProfileDatabase(const char *mappings, int count) {
    if (mappings == NULL || count <= 0) return;

    char dir[512];
    snprintf(dir, sizeof dir, "%s/profiles", rv_paths_data());
    mkdir(dir, 0777);

    char path[600];
    snprintf(path, sizeof path, "%s/gamecontrollerdb.txt", dir);

    struct stat info;
    if (stat(path, &info) == 0) {
        rvnx_log_print(4, "input", "%s exists, leaving it alone", path);
        return;
    }

    FILE *file = fopen(path, "w");
    if (file == NULL) {
        rvnx_log_print(5, "input", "could not write %s", path);
        return;
    }

    fputs("# Written by revoltnx on first run.\n"
          "# RVGL's bundled databases carry no platform:Switch entries, so none\n"
          "# of them can apply here. Edit or delete this file freely; it is only\n"
          "# recreated when missing.\n", file);
    fputs(mappings, file);
    fclose(file);

    rvnx_log_print(4, "input", "seeded %s with %d mapping(s)", path, count);
}

/* ------------------------------------------------------------------ */
/* Text input                                                          */
/* ------------------------------------------------------------------ */
/*
 * Hardware says the software keyboard WORKS -- multiplayer address entry
 * accepts text and the engine receives it -- but is glitchy on the profile
 * name screen. That difference is the whole diagnosis: the applet is fine, so
 * something specific to the name screen is upsetting it.
 *
 * The likely cause is repeat calls. SDL's contract is that StartTextInput is
 * idempotent -- calling it while text input is already active is a no-op --
 * but switch-sdl2 launches the applet on each call. A screen that calls it
 * once behaves; a screen that calls it every frame relaunches the keyboard
 * continuously, which is exactly what "glitchy" looks like.
 *
 * So the default is to DEBOUNCE rather than suppress: the first call reaches
 * SDL, repeats while already active do not. Multiplayer entry is unaffected
 * because it was never the repeating case, and the name screen stops
 * relaunching. The call counts go in the log, which will confirm or refute
 * this outright.
 *
 * nokeyboard.flag beside the NRO suppresses the applet entirely instead, for
 * anyone who would rather use Re-Volt's lettered wheel for names. That costs
 * free-text entry everywhere, including multiplayer.
 */

enum { RV_TEXT_DEBOUNCE = 0, RV_TEXT_SUPPRESS = 1 };

#ifndef RVNX_TEXT_MODE
#define RVNX_TEXT_MODE RV_TEXT_DEBOUNCE
#endif

static int g_textMode = RVNX_TEXT_MODE;
static int g_textActive;      /* what RVGL sees */
/* swkbdShow blocks, so this guards against a call arriving while the applet
 * is already up. Declared here because RunSoftwareKeyboard below uses it. */
static int g_keyboardOpen;
static unsigned g_startCalls, g_repeatCalls;

void rv_input_configure_text(const char *baseDir) {
    if (baseDir != NULL) {
        char path[512];
        struct stat info;

        snprintf(path, sizeof path, "%s/nokeyboard.flag", baseDir);
        if (stat(path, &info) == 0) g_textMode = RV_TEXT_SUPPRESS;
    }

    rvnx_log_print(4, "input", "text input: %s",
                   g_textMode == RV_TEXT_SUPPRESS
                       ? "keyboard suppressed, use the in-game wheel"
                       : "keyboard on, repeat starts debounced");
}

/* Running the keyboard ourselves.
 *
 * Two things the hardware log makes clear:
 *
 *   RVGL cycles Stop -> Start -> Stop -> Start rather than calling Start
 *   repeatedly, so the earlier debounce never fired -- each Start arrived with
 *   text input inactive and legitimately reopened the applet.
 *
 *   And erasing cannot work through SDL at all. swkbd is modal: it returns a
 *   FINISHED STRING, never keystrokes. RVGL builds its field from
 *   SDL_TEXTINPUT events and deletes on SDLK_BACKSPACE, so a deletion made
 *   inside the applet has no way to reach it. Text only ever accumulates.
 *
 * So the applet is driven here instead of by SDL. On the first Start we show
 * it once, then replace RVGL's field wholesale: a run of backspaces to clear
 * whatever it holds, then the result as text. Editing inside the applet then
 * behaves, because the whole string is resent rather than appended.
 */

#define RV_KEYBOARD_COOLDOWN_MS 400
#define RV_KEYBOARD_MAX_TEXT    64

static Uint32 g_keyboardClosedAt;

/* The text currently in the field, rebuilt from what the engine is handed.
 * It seeds the applet so there is something to edit, and sets how many
 * backspaces a replacement needs. */
static char g_fieldText[RV_KEYBOARD_MAX_TEXT];

/* Whether a full session has been observed. Until one has, the field may hold
 * text from before this process started and a count based on the tracker
 * would be short -- so the first replacement clears the maximum. */
static int g_fieldKnown;

/* The window our synthetic events claim to come from.
 *
 * The log proves the backspaces reach SDL -- 128 of them logged by the watcher
 * -- and equally proves RVGL ignores every one, because the field kept
 * accumulating. The only respect in which our events differ from the ones SDL
 * produces itself is windowID: SDL stamps the focused window, and a zeroed
 * event carries 0. An engine that checks the field before acting on a key will
 * drop ours and keep the text.
 *
 * SDL_TEXTINPUT arrived with windowID 0 and was accepted, so the check is not
 * universal -- but text is the path that already worked, and keys are the one
 * that did not. */
static Uint32 WindowId(void) {
    SDL_Window *window = SDL_GetKeyboardFocus();
    if (window == NULL) window = SDL_GL_GetCurrentWindow();
    return window != NULL ? SDL_GetWindowID(window) : 0;
}

/* The tracked field, rebuilt from what the engine is actually handed.
 *
 * Observation moved here from SDL's event watcher when delivery moved to
 * rv_events.c: the watcher only sees events that go through SDL's queue, and
 * synthetic input no longer does. Being called from rv_poll_event is strictly
 * better anyway -- it sees exactly what RVGL consumed, by the same path,
 * rather than what SDL was asked to enqueue. */
void rv_input_observe_event(const SDL_Event *event) {
    if (event == NULL) return;

    size_t length = strlen(g_fieldText);

    if (event->type == SDL_TEXTINPUT) {
        const size_t room = sizeof g_fieldText - 1 - length;
        const size_t adding = strlen(event->text.text);
        if (adding <= room) {
            memcpy(g_fieldText + length, event->text.text, adding);
            g_fieldText[length + adding] = '\0';
        }
        return;
    }

    if (event->type == SDL_KEYDOWN &&
        event->key.keysym.sym == SDLK_BACKSPACE && length > 0) {
        /* Drop a whole character, not a byte: the applet returns UTF-8 and
         * half a codepoint left behind corrupts every later comparison. */
        do { --length; } while (length > 0 &&
                                (g_fieldText[length] & 0xC0) == 0x80);
        g_fieldText[length] = '\0';
    }
}

/* PushKey/PushText are gone. Both went through SDL_PushEvent, which put
 * SDL's filter, its per-type enable state and the engine's own windowID
 * checks between us and RVGL -- and the log showed 128 backspaces surviving
 * all the way to SDL's watcher and still being ignored. rv_events.c hands
 * them to RVGL's poll loop directly instead. */

static void RunSoftwareKeyboard(void) {
#ifdef __SWITCH__
    if (g_keyboardOpen) return;
    g_keyboardOpen = 1;

    SwkbdConfig config;
    if (R_FAILED(swkbdCreate(&config, 0))) {
        rvnx_log_print(6, "input", "swkbdCreate failed; falling back to SDL");
        g_keyboardOpen = 0;
        SDL_StartTextInput();
        return;
    }

    swkbdConfigMakePresetDefault(&config);
    swkbdConfigSetStringLenMax(&config, RV_KEYBOARD_MAX_TEXT - 1);

    /* Seed the applet with what is already in the field, so editing and
     * deleting happen inside the keyboard where they work naturally. Without
     * this the applet opens empty and there is nothing to erase. */
    if (g_fieldText[0] != '\0')
        swkbdConfigSetInitialText(&config, g_fieldText);

    char result[RV_KEYBOARD_MAX_TEXT];
    result[0] = '\0';
    const Result rc = swkbdShow(&config, result, sizeof result);
    swkbdClose(&config);

    g_keyboardOpen = 0;
    g_keyboardClosedAt = SDL_GetTicks();
    if (R_FAILED(rc)) return;          /* cancelled: leave the field alone */

    /* Send the DIFFERENCE, not a clear and a retype.
     *
     * On Android the keyboard is not modal: SDL delivers real keystrokes,
     * backspace included, and RVGL deletes a character at a time. The Switch
     * applet hands back a finished string, so the edit has to be reconstructed
     * -- find the common prefix, backspace away the rest, type the remainder.
     *
     * "PKOY" -> "PKAY" becomes two backspaces and "AY", not 64 backspaces and
     * four characters. That matters: a menu that consumes one input event per
     * frame will drop a long burst, which is exactly how a blind clear loses
     * the text it was supposed to replace.
     */
    char previous[RV_KEYBOARD_MAX_TEXT];
    snprintf(previous, sizeof previous, "%s", g_fieldText);

    size_t prefix = 0;
    while (previous[prefix] != '\0' && result[prefix] != '\0' &&
           previous[prefix] == result[prefix])
        ++prefix;

    /* ALWAYS a full clear, never a delta.
     *
     * A delta is only correct while the tracked string matches RVGL's field
     * exactly, and there are two ways for those to drift that nothing here can
     * observe: RVGL enforces its own maximum length and silently discards
     * anything past it, and it can reset the field when the screen changes
     * without emitting an event. Either one leaves the tracker longer than the
     * field, and then a delta computes a common prefix that does not exist --
     * sending too few backspaces and typing only a suffix, which is exactly
     * the accumulation this was supposed to end.
     *
     * Clearing outright removes the whole class. Backspacing an empty field is
     * a no-op, so an over-generous count costs a handful of queued events and
     * makes the outcome independent of whether the tracker was right.
     *
     * The tracker still earns its place: it seeds the applet so there is
     * something to edit. If it is wrong the seed text looks wrong, which is
     * visible and fixable in the applet -- the field still ends up exactly
     * equal to whatever the applet returned. */
    (void)prefix;

    /* Now that delivery is guaranteed and ordered, the count can follow the
     * tracked length instead of always clearing the maximum -- each backspace
     * costs a frame, so 64 of them would be a visible second of the field
     * emptying itself. A small margin covers the tracker being slightly
     * behind. */
    size_t backspaces = strlen(previous) + 4;
    if (backspaces > RV_KEYBOARD_MAX_TEXT) backspaces = RV_KEYBOARD_MAX_TEXT;
    if (!g_fieldKnown) backspaces = RV_KEYBOARD_MAX_TEXT;

    for (size_t i = 0; i < backspaces; ++i)
        rv_events_queue_key(SDLK_BACKSPACE, SDL_SCANCODE_BACKSPACE);

    rv_events_queue_text(result);
    g_fieldKnown = 1;

    /* No assignment to g_fieldText here. The backspaces and text just pushed
     * go through SDL's queue, the watcher sees them, and the field updates
     * itself -- the same path any other input takes. Writing it directly
     * would give two sources of truth that could disagree. */
    rvnx_log_print(4, "input", "keyboard: '%s' -> '%s' (%zu backspaces queued, %zu typed)",
                   previous, result, backspaces, strlen(result));
#else
    SDL_StartTextInput();
#endif
}

/* Restructured after the previous version broke text entry outright. Three
 * faults, found by comparing against the reference port's editbox.c:
 *
 *   1. The cooldown path returned WITHOUT setting g_textActive. RVGL gates on
 *      SDL_IsTextInputActive, so for the whole cooldown it believed text input
 *      was off. That is the one that broke it.
 *
 *   2. g_sdlActive was set but SDL_StartTextInput was never called in this
 *      mode, so the matching SDL_StopTextInput stopped something that had
 *      never started. The port owns the applet here; SDL should not be in the
 *      path at all.
 *
 *   3. Re-entry was unguarded. swkbdShow BLOCKS -- it runs a system applet --
 *      so a second call arriving while one is up must be dropped, exactly as
 *      editbox.c does with its `if (g_editbox_open) return;`.
 *
 * The flag is now set unconditionally and first, before any decision about
 * whether to show anything. Whatever else happens, RVGL's view of its own
 * state stays correct.
 */


void rv_text_input_start(void) {
    ++g_startCalls;

    /* Unconditional and first. Every early return below leaves this set. */
    const int wasActive = g_textActive;
    g_textActive = 1;

    if (g_textMode == RV_TEXT_SUPPRESS) {
        if (!wasActive)
            rvnx_log_print(4, "input", "text entry open -- keyboard suppressed");
        return;
    }

    /* swkbdShow blocks, so this catches a call arriving from a callback while
     * the applet is up rather than a repeat in the normal flow. */
    if (g_keyboardOpen) {
        ++g_repeatCalls;
        return;
    }

    /* RVGL cycles Stop -> Start. Without a short guard the applet reopens the
     * instant it is dismissed and there is no way out of the screen. Short
     * enough not to block a deliberate reopen. */
    const Uint32 now = SDL_GetTicks();
    if (g_keyboardClosedAt != 0 && now - g_keyboardClosedAt < RV_KEYBOARD_COOLDOWN_MS) {
        ++g_repeatCalls;
        return;
    }

    rvnx_log_print(4, "input", "text entry open -- showing the keyboard");
    RunSoftwareKeyboard();
}

void rv_text_input_stop(void) {
    if (!g_textActive) return;
    g_textActive = 0;

    /* SDL is deliberately not called: it was never started in this mode.
     * The tracked field survives -- RVGL cycles Stop/Start between sessions,
     * and clearing here is what made the applet open empty and append. */
    rv_input_rescan_profiles();

    if (g_repeatCalls > 0) {
        rvnx_log_print(4, "input", "text entry closed: %u starts, %u held back",
                       g_startCalls, g_repeatCalls);
        g_repeatCalls = 0;
    }
}

SDL_bool rv_is_text_input_active(void) {
    /* Answer from our own flag in both modes. In suppress mode SDL was never
     * started; in debounce mode SDL and we agree, but RVGL must not see the
     * state flicker between a repeat call and the applet actually closing. */
    return g_textActive ? SDL_TRUE : SDL_FALSE;
}

#ifndef RVNX_WATCH_INPUT
/* Off: it existed to establish whether SDL was producing input events at all,
 * and it answered that. The watcher itself is still installed either way --
 * it also maintains the tracked text field, which is not diagnostics -- but
 * the logging half is quiet now. Build with RVNX_WATCH_INPUT=1 to bring it
 * back if input ever needs looking at again. */
#define RVNX_WATCH_INPUT 0
#endif

#define RVNX_WATCH_LIMIT 300

/* Defined with the profile code further down; the watcher drives it. */
static void MaybeRescan(void);

static int EventWatch(void *userdata, SDL_Event *event) {
    (void)userdata;

    /* Before anything that can return early. Tracking is not diagnostics: if
     * it stopped at the 300-event log cap the field would silently drift. */

    MaybeRescan();

#if !RVNX_WATCH_INPUT
    return 0;
#endif

    static int seen;
    if (seen >= RVNX_WATCH_LIMIT) return 0;

    switch (event->type) {
        case SDL_TEXTINPUT:
            ++seen;
            rvnx_log_print(4, "watch", "TEXTINPUT '%s' (window %u)",
                           event->text.text, event->text.windowID);
            break;

        case SDL_TEXTEDITING:
            ++seen;
            rvnx_log_print(4, "watch", "TEXTEDITING '%s' start %d length %d",
                           event->edit.text, event->edit.start, event->edit.length);
            break;

        case SDL_KEYDOWN:
            ++seen;
            rvnx_log_print(4, "watch", "KEYDOWN scancode %d keycode %d '%s'",
                           event->key.keysym.scancode, (int)event->key.keysym.sym,
                           SDL_GetKeyName(event->key.keysym.sym));
            break;

        case SDL_CONTROLLERBUTTONDOWN:
            ++seen;
            rvnx_log_print(4, "watch", "PAD %d button %d '%s' down",
                           (int)event->cbutton.which, event->cbutton.button,
                           SDL_GameControllerGetStringForButton(
                               (SDL_GameControllerButton)event->cbutton.button));
            break;

        case SDL_JOYBUTTONDOWN:
            ++seen;
            rvnx_log_print(4, "watch", "JOY %d button %d down (raw, not a controller)",
                           (int)event->jbutton.which, event->jbutton.button);
            break;

        case SDL_CONTROLLERAXISMOTION:
            /* Only the extremes, or a resting stick fills the log. */
            if (event->caxis.value > 24000 || event->caxis.value < -24000) {
                ++seen;
                rvnx_log_print(4, "watch", "PAD %d axis %d = %d",
                               (int)event->caxis.which, event->caxis.axis,
                               event->caxis.value);
            }
            break;

        case SDL_CONTROLLERDEVICEADDED:
        case SDL_CONTROLLERDEVICEREMOVED:
            ++seen;
            rvnx_log_print(4, "watch", "controller device %s: %d",
                           event->type == SDL_CONTROLLERDEVICEADDED ? "added" : "removed",
                           (int)event->cdevice.which);
            break;

        default:
            break;
    }

    if (seen == RVNX_WATCH_LIMIT) {
        ++seen;
        rvnx_log_print(4, "watch", "-- %d events logged, watcher quiet from here --",
                       RVNX_WATCH_LIMIT);
    }

    return 0;   /* never consume */
}

/* RVGL's controller settings, corrected against a real profile.ini.
 *
 * The first attempt at this wrote "profiles/rvgl.ini" containing
 * "Joystick 1" -- wrong file, wrong format, wrong value. The real file is an
 * INI with sections:
 *
 *     [Controller1]
 *     Joystick = 0            <- DEVICE INDEX. -1 is none, 0 is the first pad.
 *     ForceFeedback = 0
 *     SteeringDeadzone = 10
 *
 * So "1" did not mean "enabled", it meant "the second joystick". Guessing the
 * value from where the key sat in the string table was the mistake; the file
 * RVGL writes is the only authority.
 *
 * This no longer creates anything. RVGL writes profile.ini itself, under a
 * profile directory named after a player who does not exist until the name
 * screen has been used, so there is nothing to create ahead of time. Instead
 * it finds the file RVGL already wrote and corrects only the two fields that
 * decide whether the pad is used, and only when they are wrong. Every other
 * line, including all the key bindings, is passed through untouched.
 */

static char g_lastChange[64];

static int PatchControllerSection(const char *path) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) return 0;

    static char text[64 * 1024];
    const size_t length = fread(text, 1, sizeof text - 1, file);
    fclose(file);
    if (length == 0 || length >= sizeof text - 1) return 0;
    text[length] = '\0';

    /* Rebuilt line by line rather than patched in place. An in-place edit has
     * to keep the field the same width, which means writing "Joystick =  0"
     * with two spaces -- valid, but it leaves a fingerprint in a file the user
     * may open. This reproduces every other line byte for byte. */
    static char out[64 * 1024];
    size_t used = 0;
    int inController1 = 0;
    int changed = 0;

    /* Two passes. The first decides whether this profile has no controller at
     * all; only then is the touch overlay turned off with it.
     *
     * The Switch has a touchscreen, so an overlay on a profile that already
     * has a pad assigned is a deliberate choice and is left alone. Forcing it
     * off unconditionally rewrote an existing, working profile during
     * testing -- which is how this distinction got found. */
    const int noController = strstr(text, "[Controller1]") != NULL &&
        (strstr(strstr(text, "[Controller1]"), "Joystick = -1") != NULL) &&
        (strstr(text, "\n[") == NULL ||
         strstr(strstr(text, "[Controller1]"), "Joystick = -1") <
         strstr(strstr(text, "[Controller1]") + 1, "\n["));

    int assignedPad = 0, hidTouch = 0;

    const char *cursor = text;
    while (*cursor != '\0') {
        const char *end = strchr(cursor, '\n');
        const size_t lineLength = (end != NULL) ? (size_t)(end - cursor) + 1
                                                : strlen(cursor);

        /* Section headers. Only the first controller drives the menus, and
         * rewriting the others would take a second player's setup with it. */
        if (cursor[0] == '[')
            inController1 = (strncmp(cursor, "[Controller1]", 13) == 0);

        int replaced = 0;

        /* The Android build defaults a new profile to its on-screen touch
         * controls: no joystick assigned, and the touch buttons drawn at 50%
         * opacity. There is no touchscreen here, so a fresh profile is
         * unplayable until both are corrected. */
        if (noController && inController1 &&
            strncmp(cursor, "ButtonOpacity", 13) == 0 &&
            strncmp(cursor, "ButtonOpacity = 0", 17) != 0) {
            used += (size_t)snprintf(out + used, sizeof out - used,
                                     "ButtonOpacity = 0\n");
            replaced = 1;
            changed = 1;
            hidTouch = 1;
        }

        if (!replaced && inController1 && strncmp(cursor, "Joystick", 8) == 0) {
            const char *value = strchr(cursor, '=');
            /* Only when no device is assigned. An existing choice is the
             * user's and is never overridden. */
            if (value != NULL && strstr(value, "-1") != NULL &&
                (size_t)(strstr(value, "-1") - cursor) < lineLength) {
                used += (size_t)snprintf(out + used, sizeof out - used,
                                         "Joystick = 0\n");
                replaced = 1;
                changed = 1;
                assignedPad = 1;
            }
        }

        if (!replaced) {
            if (used + lineLength >= sizeof out) return 0;
            memcpy(out + used, cursor, lineLength);
            used += lineLength;
        }

        if (end == NULL) break;
        cursor = end + 1;
    }

    if (!changed) return 0;

    file = fopen(path, "wb");
    if (file == NULL) return 0;
    fwrite(out, 1, used, file);
    fclose(file);

    /* Say what was done. The previous message claimed "Joystick -1 -> 0"
     * whenever anything changed, which was untrue the moment a second field
     * could be rewritten on its own. */
    snprintf(g_lastChange, sizeof g_lastChange, "%s%s%s",
             assignedPad ? "pad assigned" : "",
             (assignedPad && hidTouch) ? ", " : "",
             hidTouch ? "touch overlay hidden" : "");
    return 1;
}

void rv_input_seed_settings(const char *dataDir) {
    if (dataDir == NULL) return;

    char profiles[512];
    snprintf(profiles, sizeof profiles, "%s/profiles", dataDir);

    DIR *dir = opendir(profiles);
    if (dir == NULL) return;

    int patched = 0, seen = 0;
    for (;;) {
        const struct dirent *entry = readdir(dir);
        if (entry == NULL) break;
        if (entry->d_name[0] == '.') continue;

        /* 512 + 256 + "/profile.ini" -- sized so the compiler can prove it
         * fits rather than warn about truncating a path silently. */
        char path[832];
        snprintf(path, sizeof path, "%s/%s/profile.ini", profiles, entry->d_name);

        struct stat info;
        if (stat(path, &info) != 0) continue;

        ++seen;
        if (PatchControllerSection(path)) {
            ++patched;
            rvnx_log_print(4, "input", "%s: %s", entry->d_name, g_lastChange);
        }
    }
    closedir(dir);

    /* Only the first pass reports "nothing to do"; the repeats are silent or
     * they would fill the log every two seconds. */
    static int reported;
    if (patched == 0 && !reported) {
        reported = 1;
        if (seen == 0)
            rvnx_log_print(4, "input", "no profiles yet -- RVGL writes one after "
                                       "the name screen");
        else
            rvnx_log_print(4, "input", "%d profile(s), controller already assigned",
                           seen);
    }
}

/* ------------------------------------------------------------------ */
/* Catching a profile as RVGL writes it                                */
/* ------------------------------------------------------------------ */
/*
 * The boot-time pass only sees profiles that already exist. A profile created
 * during the session -- which is every first run, and every "start a new
 * game" after that -- is written after it has run, so it keeps the Android
 * defaults until the next launch. That is the whole complaint: new profiles
 * come up on touch controls.
 *
 * So the file is corrected when RVGL closes it. These two wrappers only
 * observe: they delegate unconditionally and return what the real call
 * returned. Nothing here can make a file operation fail, which is the
 * distinction from the index interception that black-screened an earlier
 * build -- that one fabricated ENOENT.
 */

#define RV_TRACKED 8

static pthread_mutex_t g_profileLock = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_trackedFile[RV_TRACKED];
static char g_trackedPath[RV_TRACKED][512];

/* Any .ini under profiles/, not just the exact name.
 *
 * The first version matched "profile.ini" exactly and never fired, because
 * RVGL does not write the file under its final name -- the hardware log shows
 * a profile created with no "new profile:" line anywhere. A save written to a
 * temporary and renamed into place is the usual reason, and RVGL does import
 * rename. Matching the directory instead of the filename covers that. */
static int PathIsProfile(const char *path) {
    if (path == NULL) return 0;
    if (strstr(path, "/profiles/") == NULL) return 0;

    const size_t length = strlen(path);
    return length >= 4 && strcmp(path + length - 4, ".ini") == 0;
}

FILE *rv_profile_fopen(const char *path, const char *mode) {
    FILE *file = fopen_fake(path, mode);

    if (file != NULL && mode != NULL && mode[0] != 'r' && PathIsProfile(path)) {
        pthread_mutex_lock(&g_profileLock);
        for (int i = 0; i < RV_TRACKED; ++i) {
            if (g_trackedFile[i] != NULL) continue;
            g_trackedFile[i] = file;
            snprintf(g_trackedPath[i], sizeof g_trackedPath[i], "%s", path);
            break;
        }
        pthread_mutex_unlock(&g_profileLock);
    }

    return file;
}

int rv_profile_fclose(FILE *file) {
    char path[512];
    path[0] = '\0';

    pthread_mutex_lock(&g_profileLock);
    for (int i = 0; i < RV_TRACKED; ++i) {
        if (g_trackedFile[i] != file) continue;
        snprintf(path, sizeof path, "%s", g_trackedPath[i]);
        g_trackedFile[i] = NULL;
        break;
    }
    pthread_mutex_unlock(&g_profileLock);

    const int result = fclose_fake(file);

    /* After the close, so the contents are on the card before they are read
     * back and rewritten. */
    if (result == 0 && path[0] != '\0' && PatchControllerSection(path))
        rvnx_log_print(4, "input", "new profile: %s (%s)", path, g_lastChange);

    return result;
}

int rv_profile_rename(const char *from, const char *to) {
    const int result = rename(from, to);
    if (result == 0 && PathIsProfile(to) && PatchControllerSection(to))
        rvnx_log_print(4, "input", "profile renamed into place: %s (%s)",
                       to, g_lastChange);
    return result;
}

/* Belt and braces.
 *
 * fclose and rename cover the routes we know about, but the file did not get
 * caught on hardware and guessing a third route would be guessing again. A
 * profile is created exactly once, right after a name is confirmed, so the
 * scan is re-run when text entry closes -- and again on a timer for a while
 * afterwards, in case the write trails the close.
 *
 * Bounded: it stops after the window expires. PatchControllerSection returns
 * without writing when there is nothing to change, so a scan that finds
 * everything in order costs one small read per profile. */
#define RV_RESCAN_WINDOW_MS 60000
#define RV_RESCAN_EVERY_MS   2000

static Uint32 g_lastRescan;
static Uint32 g_firstRescan;

void rv_input_rescan_profiles(void) {
    rv_input_seed_settings(rv_paths_data());
}

static void MaybeRescan(void) {
    const Uint32 now = SDL_GetTicks();
    if (g_firstRescan == 0) g_firstRescan = now;
    if (now - g_firstRescan > RV_RESCAN_WINDOW_MS) return;
    if (g_lastRescan != 0 && now - g_lastRescan < RV_RESCAN_EVERY_MS) return;

    g_lastRescan = now;
    rv_input_rescan_profiles();
}

void rv_input_install_event_watch(void) {
    /* Always installed: it maintains g_fieldText as well as logging, and
     * RVNX_WATCH_INPUT only gates the logging half. */
    SDL_AddEventWatch(EventWatch, NULL);
#if RVNX_WATCH_INPUT
    rvnx_log_print(4, "watch", "input event watcher active (first %d events)",
                   RVNX_WATCH_LIMIT);
#endif
}

int rv_input_install_mappings(void) {
    const int userMappings = LoadUserOverrides();

    const int count = SDL_NumJoysticks();
    SDL_version linked;
    SDL_GetVersion(&linked);

    rvnx_log_print(4, "input", "SDL %d.%d.%d, platform '%s', %d joystick(s)",
                   linked.major, linked.minor, linked.patch,
                   SDL_GetPlatform(), count);

    if (count <= 0)
        rvnx_log_print(5, "input",
                       "no pads attached at startup -- one plugged in later will "
                       "not be mapped until the next launch");

    char collected[2048];
    size_t collectedUsed = 0;
    collected[0] = '\0';

    int added = 0;

    for (int i = 0; i < count; ++i) {
        const SDL_JoystickGUID guid = SDL_JoystickGetDeviceGUID(i);

        char guidText[64] = {0};
        SDL_JoystickGetGUIDString(guid, guidText, sizeof guidText);

        const char *name = SDL_JoystickNameForIndex(i);

        char *existing = SDL_GameControllerMappingForGUID(guid);
        if (existing != NULL) {
            rvnx_log_print(4, "input", "pad %d '%s' %s already mapped",
                           i, name ? name : "?", guidText);
            SDL_free(existing);
            continue;
        }

        SDL_Joystick *stick = SDL_JoystickOpen(i);
        if (stick == NULL) {
            rvnx_log_print(6, "input", "pad %d: SDL_JoystickOpen failed: %s",
                           i, SDL_GetError());
            continue;
        }

        const int numAxes = SDL_JoystickNumAxes(stick);
        const int numButtons = SDL_JoystickNumButtons(stick);
        const int numHats = SDL_JoystickNumHats(stick);
        SDL_JoystickClose(stick);

        rvnx_log_print(4, "input", "pad %d '%s' %s: axes %d, buttons %d, hats %d",
                       i, name ? name : "?", guidText, numAxes, numButtons, numHats);

        RvPadShape shape;
        char mapping[512];
        const size_t length = rv_input_build_mapping(
            guidText, name, SDL_GetPlatform(), !RVNX_POSITIONAL_FACE_BUTTONS,
            numAxes, numButtons, numHats, &shape, mapping, sizeof mapping);

        if (length == 0) {
            rvnx_log_print(6, "input",
                           "pad %d: cannot build a mapping for this shape", i);
            continue;
        }

        if (SDL_GameControllerAddMapping(mapping) < 0) {
            rvnx_log_print(6, "input", "pad %d: SDL rejected the mapping: %s",
                           i, SDL_GetError());
            continue;
        }

        ++added;
        rvnx_log_print(4, "input",
                       "pad %d mapped: %s faces, %s triggers, %s dpad, right stick %s",
                       i,
                       RVNX_POSITIONAL_FACE_BUTTONS ? "positional" : "Nintendo",
                       shape.analogueTriggers ? "analogue" : "digital",
                       shape.hatDpad ? "hat" : (shape.buttonDpad ? "buttons" : "NONE"),
                       shape.rightStick ? "yes" : "NO");

        if (!SDL_IsGameController(i))
            rvnx_log_print(6, "input",
                           "pad %d is STILL not a game controller after mapping "
                           "-- RVGL will fall back to raw joystick", i);

        const int written = snprintf(collected + collectedUsed,
                                     sizeof collected - collectedUsed,
                                     "%s\n", mapping);
        if (written > 0 && (size_t)written < sizeof collected - collectedUsed)
            collectedUsed += (size_t)written;
    }

    /* Survives a game-controller subsystem restart: SDL re-reads this hint on
     * every init, whereas AddMapping's list is freed by SDL_GameControllerQuit. */
    if (collectedUsed > 0) SDL_SetHint(SDL_HINT_GAMECONTROLLERCONFIG, collected);

    SeedProfileDatabase(collected, added);

    rvnx_log_print(4, "input", "%d mapping(s) added, %d from user file",
                   added, userMappings);
    return added;
}
