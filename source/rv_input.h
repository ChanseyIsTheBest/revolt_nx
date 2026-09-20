/* rv_input.h -- make the Switch pad visible to RVGL as a game controller.
 *
 * THE PROBLEM
 * -----------
 * RVGL reads gamepads through SDL_GameController, and loads its own
 * gamecontrollerdb.txt at startup via SDL_GameControllerAddMappingsFromRW.
 * That database ships 1909 entries and not one of them is usable here: every
 * line carries a `platform:` field, and the only values present are Android,
 * Linux, Mac OS X, Windows and iOS. SDL_GetPlatform() on switch-sdl2 returns
 * "Switch", and SDL rejects any mapping whose platform does not match.
 *
 * So without this file, SDL_IsGameController() is false for every pad, RVGL
 * falls back to raw SDL_Joystick, and the buttons land in an arbitrary order.
 * The game boots, the pad enumerates, and nothing does what it says. That is
 * a worse failure than not booting, because it looks like the port works.
 *
 * THE FIX
 * -------
 * Register a platform:Switch mapping for each attached pad before RVGL runs.
 * The GUID is read from SDL at runtime rather than hardcoded, so handheld
 * mode, a Pro Controller, a single Joy-Con and a second player's pad each get
 * a correct entry whatever GUID the driver synthesises for them.
 *
 * MIT licensed.
 */
#ifndef RVNX_RV_INPUT_H
#define RVNX_RV_INPUT_H

#include <stddef.h>
#include <stdio.h>

#include <SDL2/SDL.h>

/* Text input: keep RVGL's state machine, lose the applet.
 *
 * RVGL calls SDL_StartTextInput when its name-entry screen opens, and
 * switch-sdl2 answers that by launching the Switch software keyboard. On
 * hardware the applet appears, accepts text, and the text never reaches the
 * engine -- so the keyboard blocks the screen without being able to fill it.
 *
 * Hardware says the applet WORKS -- multiplayer address entry receives text --
 * but is glitchy on the profile name screen. SDL's contract says
 * StartTextInput is idempotent; switch-sdl2 relaunches the applet on each
 * call. A screen calling it every frame therefore reopens the keyboard
 * continuously.
 *
 * Default is to debounce: the first call reaches SDL, repeats while already
 * active do not. IsTextInputActive answers from a flag of our own so RVGL
 * never sees its state machine flicker.
 *
 * nokeyboard.flag beside the NRO suppresses the applet entirely instead, for
 * anyone preferring Re-Volt's lettered wheel. That costs free text everywhere,
 * multiplayer included. */
void rv_input_configure_text(const char *baseDir);

void      rv_text_input_start(void);
void      rv_text_input_stop(void);
SDL_bool  rv_is_text_input_active(void);

/* Seeds <data>/profiles/rvgl.ini so a first run comes up on the pad rather
 * than the keyboard. Written ONLY when the file is absent -- RVGL owns it
 * afterwards and overwriting a user's settings every boot would be worse than
 * not seeding at all. */
void rv_input_seed_settings(const char *dataDir);

/* fopen/fclose wrappers that correct a profile RVGL writes during the session.
 * They only observe -- both delegate unconditionally and return what the real
 * call returned, so neither can make a file operation fail. */
void rv_input_observe_event(const SDL_Event *event);

FILE *rv_profile_fopen(const char *path, const char *mode);
int   rv_profile_fclose(FILE *file);
int   rv_profile_rename(const char *from, const char *to);

/* Re-runs the profile check. Cheap when nothing needs changing. */
void  rv_input_rescan_profiles(void);

/* Installs an SDL event watcher that logs input events as SDL delivers them.
 *
 * Non-invasive: SDL_AddEventWatch sees every event without consuming it, so
 * RVGL's own SDL_PollEvent loop is untouched. This exists to answer one
 * question that cannot be answered from outside -- when the software keyboard
 * closes, does SDL actually produce SDL_TEXTINPUT, and do the pads produce
 * controller events at all?
 *
 * Bounded, and off unless RVNX_WATCH_INPUT is set. */
void rv_input_install_event_watch(void);

/* Registers mappings for every attached pad. Call after SDL_Init and before
 * handing control to the engine. Returns how many were added. */
int rv_input_install_mappings(void);

/* What shape of pad the mapping was built for. Logged at boot so a hardware
 * report says which branch ran, rather than leaving it to be guessed. */
typedef struct {
    int numAxes, numButtons, numHats;
    int analogueTriggers;   /* ZL/ZR came back as axes 4/5 */
    int hatDpad;            /* D-pad is a hat */
    int buttonDpad;         /* D-pad is buttons 12-15 */
    int rightStick;         /* pad has a second stick */
} RvPadShape;

/* Builds one SDL mapping line. Exposed for testing.
 *
 * nintendoFaceOrder != 0 puts SDL's A on the Switch's A (the RIGHT button),
 * which is what a Switch player expects to mean "accelerate" or "confirm".
 * Zero uses the positional convention instead, where SDL's A is the BOTTOM
 * button, i.e. the Switch's B -- correct if you would rather match an Xbox
 * pad's geometry than Nintendo's labels.
 *
 * platform must be SDL_GetPlatform()'s return value, not the literal
 * "Switch". devkitPro's SDL 2.28.5 reports "Unknown (see SDL_platform.h)",
 * and SDL discards any mapping whose platform field does not match.
 *
 * numAxes/numButtons/numHats come from SDL_JoystickNum* for the actual pad.
 * They are not cosmetic: devkitPro has shipped builds that expose ZL/ZR as
 * axes and the D-pad as a hat, and a mapping written for the wrong one fails
 * silently -- a trigger that never fires, or no D-pad at all.
 *
 * shape may be NULL. Returns the length written, or 0 if the pad cannot be
 * mapped or the buffer was too small. */
size_t rv_input_build_mapping(const char *guid, const char *name,
                              const char *platform, int nintendoFaceOrder,
                              int numAxes, int numButtons, int numHats,
                              RvPadShape *shape, char *out, size_t capacity);

#endif
