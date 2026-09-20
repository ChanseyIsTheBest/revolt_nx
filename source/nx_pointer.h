/* nx_pointer.h -- on-screen cursor drawn over the game.
 *
 * Adapted from nx_pointer.c/h in the Happy Wheels Switch port (MIT, the same
 * so-loader lineage as this loader). One change: this module no longer reads
 * the pad. input.c owns the toggle chord and the movement, because it also
 * has to emit the cursor's taps as touch events and silence the on-screen
 * button bindings while the cursor is up.
 *
 * Kept as-is is the part that is genuinely hard: the cursor.png loader, and
 * drawing an overlay inside the game's GL context without disturbing a single
 * piece of its state.
 *
 * Drop a cursor.png (up to 64x64, transparency respected) next to the .nro to
 * replace the built-in arrow.
 *
 * MIT licensed, see LICENSE.
 */
#ifndef PB_NX_POINTER_H
#define PB_NX_POINTER_H

/* Call once before the game starts: reads cursor.png from the game folder.
 * Decoding and the GL upload happen lazily on the render thread. */
void nxp_init(int screen_w, int screen_h, const char *data_dir);

/* The render size changed (docking). Cheap no-op when unchanged; the cursor
 * keeps its relative position so it does not jump to a corner. */
void nxp_set_screen(int w, int h);

/* Draw with the game's GL context current, just before the buffer swap. */
void nxp_draw(void);

int  nxp_visible(void);
void nxp_set_visible(int on);
void nxp_pos(float *x, float *y);          /* render-space pixels */
void nxp_move(float dx, float dy);         /* clamped to the screen */

/* 1 = shader built, 0 = not built yet, -1 = compile/link failed. */
int  nxp_gl_state(void);

#endif
