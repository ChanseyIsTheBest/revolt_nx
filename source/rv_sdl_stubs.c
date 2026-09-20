/* rv_sdl_stubs.c -- the Android-only SDL2 entry points RVGL imports.
 *
 * There are exactly two, confirmed against libmain.so's import table:
 *
 *     SDL_AndroidGetExternalStoragePath
 *     SDL_AndroidRequestPermission
 *
 * Everything else in its 104-symbol SDL surface is plain cross-platform SDL2
 * and binds straight to switch-sdl2. That is the single reason this port is
 * small: RVGL's Android build is a normal SDL2 application with a thin Java
 * shell, not a GLSurfaceView/JNI engine like the reference ports.
 *
 * MIT licensed.
 */

#include <SDL2/SDL.h>

#include "config.h"
#include "rv_paths.h"

/* RVGL uses this to decide where to write profiles, saves and replays. Point
 * it at the data directory so everything stays inside one folder the user can
 * back up or move between consoles. */
const char *SDL_AndroidGetExternalStoragePath_stub(void) {
    return rv_paths_data();
}

/* Android runtime permissions. There is no such concept here, and the engine
 * only ever asks for storage access, which it already has.
 *
 * Returning SDL_TRUE matters: RVGL blocks on the result before touching its
 * data directory, so a false here looks exactly like a corrupt install. */
SDL_bool SDL_AndroidRequestPermission_stub(const char *permission) {
    (void)permission;
    return SDL_TRUE;
}
