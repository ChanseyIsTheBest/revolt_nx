/* rv_image.h -- SDL2_image, or a stand-in for it.
 *
 * RVGL imports exactly four symbols from SDL2_image:
 *
 *     IMG_Init  IMG_Quit  IMG_Load_RW  IMG_SavePNG_RW
 *
 * and its own data is almost entirely .bmp -- IMG_SavePNG_RW exists for the
 * screenshot key. That is a thin enough surface that requiring
 * switch-sdl2_image to be installed is a worse trade than covering it here,
 * so the build uses the real library when it is present and this shim when it
 * is not. The Makefile decides and defines RVNX_NO_SDL2_IMAGE accordingly,
 * which keeps the compile and the link from disagreeing.
 *
 * The shim decodes BMP through SDL itself and PNG through libpng, which the
 * port already links for other reasons.
 *
 * MIT licensed.
 */
#ifndef RVNX_RV_IMAGE_H
#define RVNX_RV_IMAGE_H

#include <SDL2/SDL.h>

#if defined(RVNX_NO_SDL2_IMAGE) && RVNX_NO_SDL2_IMAGE

#include <stddef.h>
#include <stdint.h>

#define IMG_INIT_JPG  0x00000001
#define IMG_INIT_PNG  0x00000002
#define IMG_INIT_TIF  0x00000004
#define IMG_INIT_WEBP 0x00000008

int          IMG_Init(int flags);
void         IMG_Quit(void);
SDL_Surface *IMG_Load_RW(SDL_RWops *src, int freesrc);
int          IMG_SavePNG_RW(SDL_Surface *surface, SDL_RWops *dst, int freedst);

/* The PNG work, factored out so it can be tested without SDL. Both return
 * non-zero on success; the caller frees *rgba / *out with free(). */
int rv_png_decode(const void *data, size_t size, int *width, int *height,
                  uint8_t **rgba);
int rv_png_encode(const uint8_t *rgba, int width, int height,
                  uint8_t **out, size_t *outSize);

#else

#include <SDL2/SDL_image.h>

#endif

#endif
