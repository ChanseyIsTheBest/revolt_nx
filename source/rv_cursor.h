/* rv_cursor.h -- an on-screen pointer, for anything the pad cannot reach.
 *
 * RVGL's Android build has touch controls and menu hit-boxes that expect a
 * finger. Most of it is reachable with the pad, but not all, and there is no
 * way back if something is only tappable.
 *
 * ZL+ZR together toggles a cursor. The left stick moves it, A taps. The tap is
 * delivered as a real SDL touch event through the same queue the keyboard
 * uses, so the engine sees a finger -- it imports no mouse function at all, so
 * touch is the only thing it would respond to.
 *
 * Drawing is nx_pointer.c, taken from the poorbunny port. The hard part there
 * is drawing an overlay inside the game's GL context without disturbing any of
 * its state; that is left exactly as it was.
 *
 * MIT licensed.
 */
#ifndef RVNX_RV_CURSOR_H
#define RVNX_RV_CURSOR_H

#include <SDL2/SDL.h>

void rv_cursor_init(const char *dataDir);

/* Once per frame, from the buffer-swap hook. Reads the pad, moves the cursor,
 * emits taps. */
void rv_cursor_update(void);

int rv_cursor_visible(void);

/* Pad state, neutralised while the cursor is up.
 *
 * Swallowing events is not enough on its own: RVGL POLLS the pad through
 * SDL_GameControllerGetAxis/GetButton and the joystick equivalents, so with
 * the cursor on screen the stick was still steering and the buttons still
 * firing. These report a resting pad instead, which is the only thing the
 * engine will believe.
 *
 * The cursor reads the real SDL directly, so its own toggle and movement are
 * unaffected. */
int   rv_pad_get_button(SDL_GameController *pad, SDL_GameControllerButton b);
Sint16 rv_pad_get_axis(SDL_GameController *pad, SDL_GameControllerAxis a);
Sint16 rv_joy_get_axis(SDL_Joystick *joystick, int axis);
Uint8  rv_joy_get_button(SDL_Joystick *joystick, int button);
Uint8  rv_joy_get_hat(SDL_Joystick *joystick, int hat);

/* True when this event should be withheld from the engine -- the A press that
 * is being used as a tap must not also register as a button. */
int rv_cursor_swallows(const SDL_Event *event);

/* Draws the cursor, then swaps. Wired to SDL_GL_SwapWindow. */
void rv_gl_swap_window(SDL_Window *window);

#endif
