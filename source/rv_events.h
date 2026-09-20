/* rv_events.h -- deliver synthetic input directly into RVGL's poll loop.
 *
 * WHY NOT SDL_PushEvent
 * ---------------------
 * The hardware log settled this. 128 backspace KEYDOWNs were pushed, SDL's own
 * event watcher logged every one of them, and RVGL acted on none: the text
 * field kept accumulating. Pushing into SDL's queue puts three things between
 * us and the engine -- SDL's event filter, its per-type enable state, and
 * whatever windowID checks the engine applies -- and any of them can drop an
 * event silently.
 *
 * RVGL reads input through exactly four calls: SDL_PollEvent,
 * SDL_GetKeyboardState, SDL_GetKeyFromScancode and SDL_GetModState. Sitting in
 * front of the first two means synthetic input is handed to the engine
 * directly, in the order chosen, with nothing able to discard it in between.
 *
 * PACING
 * ------
 * Key presses are delivered one per frame, with the release at the start of
 * the following frame. That is not caution, it is required: an engine that
 * edge-detects backspace by polling SDL_GetKeyboardState -- which RVGL
 * imports -- sees one press only if the key reads as held for at least one
 * whole frame. Dumping sixty backspaces into a single drain would register as
 * one keypress, or none.
 *
 * Text needs no pacing and is delivered as fast as the engine drains.
 *
 * MIT licensed.
 */
#ifndef RVNX_RV_EVENTS_H
#define RVNX_RV_EVENTS_H

#include <SDL2/SDL.h>

/* Queues a press/release pair, paced across frames. */
void rv_events_queue_key(SDL_Keycode key, SDL_Scancode scancode);

/* Queues text, split into valid UTF-8 chunks. */
void rv_events_queue_text(const char *text);

/* Queues an event verbatim, unpaced. Used for touch, where the engine needs
 * the down, the motion and the up in the order they happened. */
void rv_events_queue_raw(const SDL_Event *event);

int rv_events_pending(void);

/* The two interception points, wired in imports.c. */
int          rv_poll_event(SDL_Event *event);
const Uint8 *rv_get_keyboard_state(int *numkeys);

/* Called for every event handed to the engine, synthetic or real, so the
 * tracked field stays right whichever path the input took. */
void rv_input_observe_event(const SDL_Event *event);

#endif
