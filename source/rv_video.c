/* rv_video.c -- see rv_video.h. MIT licensed. */

#include <stdio.h>
#include <sys/stat.h>

#include "rv_video.h"
#include "rv_log.h"

#ifndef RVNX_FORCE_WIDTH
#define RVNX_FORCE_WIDTH  1920
#endif
#ifndef RVNX_FORCE_HEIGHT
#define RVNX_FORCE_HEIGHT 1080
#endif

static int g_force = 1;

void rv_video_configure(const char *baseDir) {
    if (baseDir != NULL) {
        char path[512];
        snprintf(path, sizeof path, "%s/no1080p.flag", baseDir);
        struct stat info;
        if (stat(path, &info) == 0) g_force = 0;
    }

    rvnx_log_print(4, "video", g_force ? "forcing %dx%d" : "using SDL's own size",
                   RVNX_FORCE_WIDTH, RVNX_FORCE_HEIGHT);
}

SDL_Window *rv_create_window(const char *title, int x, int y,
                             int w, int h, Uint32 flags) {
    if (!g_force) return SDL_CreateWindow(title, x, y, w, h, flags);

    SDL_Window *window = SDL_CreateWindow(title, x, y,
                                          RVNX_FORCE_WIDTH, RVNX_FORCE_HEIGHT, flags);
    if (window != NULL) {
        int actualW = 0, actualH = 0;
        SDL_GL_GetDrawableSize(window, &actualW, &actualH);
        rvnx_log_print(4, "video", "window %dx%d requested, drawable %dx%d",
                       RVNX_FORCE_WIDTH, RVNX_FORCE_HEIGHT, actualW, actualH);
        return window;
    }

    /* The driver refused it -- in handheld mode that is possible, and a NULL
     * window is a black screen with no way back. Fall through to whatever
     * RVGL originally asked for rather than lose the boot over a resolution. */
    rvnx_log_print(6, "video", "%dx%d refused (%s); falling back to %dx%d",
                   RVNX_FORCE_WIDTH, RVNX_FORCE_HEIGHT, SDL_GetError(), w, h);
    g_force = 0;
    return SDL_CreateWindow(title, x, y, w, h, flags);
}

/* RVGL picks its resolution from what SDL advertises, so the mode queries have
 * to agree with the window or its projection matrix and the drawable disagree
 * and everything renders at the wrong aspect. */
static void Override(SDL_DisplayMode *mode) {
    if (!g_force || mode == NULL) return;
    mode->w = RVNX_FORCE_WIDTH;
    mode->h = RVNX_FORCE_HEIGHT;
}

int rv_get_desktop_display_mode(int displayIndex, SDL_DisplayMode *mode) {
    const int result = SDL_GetDesktopDisplayMode(displayIndex, mode);
    if (result == 0) Override(mode);
    return result;
}

int rv_get_display_mode(int displayIndex, int modeIndex, SDL_DisplayMode *mode) {
    const int result = SDL_GetDisplayMode(displayIndex, modeIndex, mode);
    if (result == 0) Override(mode);
    return result;
}
