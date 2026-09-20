/* rv_video.h -- render at 1080p regardless of dock state.
 *
 * RVGL sizes itself once, at SDL_CreateWindow, from whatever
 * SDL_GetDesktopDisplayMode reports -- it imports no SDL_SetWindowSize, so
 * the size it gets at startup is the size it keeps. Undocked that is 1280x720.
 *
 * The Tegra is barely working at this load (7% GPU on the hardware logs), so
 * rendering at 1920x1080 and letting the console downscale to the handheld
 * panel is free supersampling, and docking mid-session no longer changes
 * anything because the render target was never tied to the panel.
 *
 * no1080p.flag beside the NRO reverts to whatever SDL reports.
 *
 * MIT licensed.
 */
#ifndef RVNX_RV_VIDEO_H
#define RVNX_RV_VIDEO_H

#include <SDL2/SDL.h>

void rv_video_configure(const char *baseDir);

SDL_Window *rv_create_window(const char *title, int x, int y,
                             int w, int h, Uint32 flags);
int rv_get_desktop_display_mode(int displayIndex, SDL_DisplayMode *mode);
int rv_get_display_mode(int displayIndex, int modeIndex, SDL_DisplayMode *mode);

#endif
