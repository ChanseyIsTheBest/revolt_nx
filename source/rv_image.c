/* rv_image.c -- see rv_image.h. Compiled only when SDL2_image is absent.
 * MIT licensed. */

#include "rv_image.h"

#if defined(RVNX_NO_SDL2_IMAGE) && RVNX_NO_SDL2_IMAGE

#include <stdlib.h>
#include <string.h>

#include <png.h>

#include "rv_log.h"

/* ------------------------------------------------------------------ */
/* PNG, over libpng                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t offset;
} PngReader;

static void ReadFromMemory(png_structp png, png_bytep out, png_size_t count) {
    PngReader *reader = (PngReader *)png_get_io_ptr(png);
    if (reader == NULL || reader->offset + count > reader->size) {
        png_error(png, "read past end of buffer");
        return;
    }
    memcpy(out, reader->data + reader->offset, count);
    reader->offset += count;
}

int rv_png_decode(const void *data, size_t size, int *width, int *height,
                  uint8_t **rgba) {
    if (data == NULL || rgba == NULL || size < 8) return 0;
    if (png_sig_cmp((png_const_bytep)data, 0, 8) != 0) return 0;

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (png == NULL) return 0;

    png_infop info = png_create_info_struct(png);
    if (info == NULL) { png_destroy_read_struct(&png, NULL, NULL); return 0; }

    uint8_t *pixels = NULL;
    png_bytep *rows = NULL;

    /* libpng reports errors by longjmp. Everything allocated above this point
     * has to be reachable from here for the cleanup to be complete. */
    if (setjmp(png_jmpbuf(png))) {
        free(pixels);
        free(rows);
        png_destroy_read_struct(&png, &info, NULL);
        return 0;
    }

    PngReader reader = { (const uint8_t *)data, size, 0 };
    png_set_read_fn(png, &reader, ReadFromMemory);
    png_read_info(png, info);

    const png_uint_32 w = png_get_image_width(png, info);
    const png_uint_32 h = png_get_image_height(png, info);
    const int depth = png_get_bit_depth(png, info);
    const int colour = png_get_color_type(png, info);

    /* Normalise everything to 8-bit RGBA so the caller has one case. */
    if (colour == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (colour == PNG_COLOR_TYPE_GRAY && depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (depth == 16) png_set_strip_16(png);
    if (colour == PNG_COLOR_TYPE_GRAY || colour == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    png_set_add_alpha(png, 0xFF, PNG_FILLER_AFTER);
    png_read_update_info(png, info);

    pixels = (uint8_t *)malloc((size_t)w * (size_t)h * 4);
    rows = (png_bytep *)malloc((size_t)h * sizeof(png_bytep));
    if (pixels == NULL || rows == NULL) png_error(png, "out of memory");

    for (png_uint_32 y = 0; y < h; ++y) rows[y] = pixels + (size_t)y * w * 4;
    png_read_image(png, rows);
    png_read_end(png, NULL);

    free(rows);
    png_destroy_read_struct(&png, &info, NULL);

    if (width != NULL) *width = (int)w;
    if (height != NULL) *height = (int)h;
    *rgba = pixels;
    return 1;
}

typedef struct {
    uint8_t *data;
    size_t size;
    size_t capacity;
    int failed;
} PngWriter;

static void WriteToMemory(png_structp png, png_bytep in, png_size_t count) {
    PngWriter *writer = (PngWriter *)png_get_io_ptr(png);
    if (writer == NULL || writer->failed) return;

    if (writer->size + count > writer->capacity) {
        size_t capacity = writer->capacity ? writer->capacity * 2 : 65536;
        while (capacity < writer->size + count) capacity *= 2;
        uint8_t *grown = (uint8_t *)realloc(writer->data, capacity);
        if (grown == NULL) { writer->failed = 1; png_error(png, "out of memory"); return; }
        writer->data = grown;
        writer->capacity = capacity;
    }

    memcpy(writer->data + writer->size, in, count);
    writer->size += count;
}

static void FlushMemory(png_structp png) { (void)png; }

int rv_png_encode(const uint8_t *rgba, int width, int height,
                  uint8_t **out, size_t *outSize) {
    if (rgba == NULL || out == NULL || width <= 0 || height <= 0) return 0;

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (png == NULL) return 0;

    png_infop info = png_create_info_struct(png);
    if (info == NULL) { png_destroy_write_struct(&png, NULL); return 0; }

    PngWriter writer = { NULL, 0, 0, 0 };
    png_bytep *rows = NULL;

    if (setjmp(png_jmpbuf(png))) {
        free(writer.data);
        free(rows);
        png_destroy_write_struct(&png, &info);
        return 0;
    }

    png_set_write_fn(png, &writer, WriteToMemory, FlushMemory);
    png_set_IHDR(png, info, (png_uint_32)width, (png_uint_32)height, 8,
                 PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

    /* Screenshots are written on a background thread while the game runs, so
     * trade ratio for time. */
    png_set_compression_level(png, 3);

    rows = (png_bytep *)malloc((size_t)height * sizeof(png_bytep));
    if (rows == NULL) png_error(png, "out of memory");
    for (int y = 0; y < height; ++y)
        rows[y] = (png_bytep)(rgba + (size_t)y * (size_t)width * 4);

    png_set_rows(png, info, rows);
    png_write_png(png, info, PNG_TRANSFORM_IDENTITY, NULL);

    free(rows);
    png_destroy_write_struct(&png, &info);

    if (writer.failed) { free(writer.data); return 0; }

    *out = writer.data;
    if (outSize != NULL) *outSize = writer.size;
    return 1;
}

/* ------------------------------------------------------------------ */
/* The SDL2_image surface                                              */
/* ------------------------------------------------------------------ */

int IMG_Init(int flags) {
    /* Claim only what we can actually do. RVGL checks the return value. */
    const int supported = IMG_INIT_PNG;
    if ((flags & ~supported) != 0)
        rvnx_log_print(5, "image",
                       "IMG_Init asked for 0x%x; this build supports PNG and BMP only",
                       flags);
    return flags & supported;
}

void IMG_Quit(void) {}

SDL_Surface *IMG_Load_RW(SDL_RWops *src, int freesrc) {
    if (src == NULL) return NULL;

    /* Sniff rather than trust the extension: RVGL passes an RWops, not a
     * path, so the extension is not available here anyway. */
    uint8_t signature[8] = {0};
    const Sint64 start = SDL_RWtell(src);
    const size_t got = SDL_RWread(src, signature, 1, sizeof signature);
    SDL_RWseek(src, start, RW_SEEK_SET);

    SDL_Surface *surface = NULL;

    if (got >= 8 && png_sig_cmp((png_const_bytep)signature, 0, 8) == 0) {
        /* PNG: pull the whole thing in, decode, wrap. */
        const Sint64 size = SDL_RWsize(src);
        if (size > 0) {
            uint8_t *buffer = (uint8_t *)malloc((size_t)size);
            if (buffer != NULL && SDL_RWread(src, buffer, 1, (size_t)size) == (size_t)size) {
                int width = 0, height = 0;
                uint8_t *rgba = NULL;
                if (rv_png_decode(buffer, (size_t)size, &width, &height, &rgba)) {
                    surface = SDL_CreateRGBSurfaceWithFormatFrom(
                        rgba, width, height, 32, width * 4, SDL_PIXELFORMAT_RGBA32);
                    if (surface != NULL) {
                        /* Hand the buffer to SDL: it frees pixels for a
                         * surface it owns, and this one does not own them. */
                        SDL_Surface *copy = SDL_ConvertSurfaceFormat(
                            surface, SDL_PIXELFORMAT_RGBA32, 0);
                        SDL_FreeSurface(surface);
                        surface = copy;
                    }
                    free(rgba);
                }
            }
            free(buffer);
        }
    } else {
        surface = SDL_LoadBMP_RW(src, 0);
    }

    if (surface == NULL)
        rvnx_log_print(5, "image", "IMG_Load_RW could not decode this image");

    if (freesrc) SDL_RWclose(src);
    return surface;
}

int IMG_SavePNG_RW(SDL_Surface *surface, SDL_RWops *dst, int freedst) {
    if (surface == NULL || dst == NULL) return -1;

    SDL_Surface *rgba = SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_RGBA32, 0);
    if (rgba == NULL) { if (freedst) SDL_RWclose(dst); return -1; }

    int result = -1;
    uint8_t *encoded = NULL;
    size_t encodedSize = 0;

    /* rv_png_encode wants tightly packed rows; SDL pads to its own pitch. */
    const int width = rgba->w;
    const int height = rgba->h;
    uint8_t *packed = (uint8_t *)malloc((size_t)width * (size_t)height * 4);

    if (packed != NULL) {
        for (int y = 0; y < height; ++y)
            memcpy(packed + (size_t)y * width * 4,
                   (const uint8_t *)rgba->pixels + (size_t)y * rgba->pitch,
                   (size_t)width * 4);

        if (rv_png_encode(packed, width, height, &encoded, &encodedSize)) {
            if (SDL_RWwrite(dst, encoded, 1, encodedSize) == encodedSize) result = 0;
            free(encoded);
        }
        free(packed);
    }

    SDL_FreeSurface(rgba);
    if (freedst) SDL_RWclose(dst);

    if (result != 0) rvnx_log_print(5, "image", "IMG_SavePNG_RW failed");
    return result;
}

#endif  /* RVNX_NO_SDL2_IMAGE */
