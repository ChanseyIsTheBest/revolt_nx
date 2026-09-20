/* mp3_decode.h -- stub.
 *
 * opensles.c can decode an MP3 for its own test tone. RVGL loads its music
 * through libmpg123.so, which we map as a real module, so this path is unused
 * and returns failure rather than dragging in a second decoder.
 *
 * MIT licensed.
 */
#ifndef RVNX_MP3_DECODE_H
#define RVNX_MP3_DECODE_H

#include <stddef.h>
#include <stdint.h>

/* Returns 1 on success, with *outPcm malloc'd (caller frees) holding
 * *outFrames interleaved frames of *outChannels samples at *outRate Hz.
 * Returns 0 and allocates nothing otherwise.
 *
 * outFrames is size_t, not int. opensles.c passes a size_t and the mismatch is
 * a hard error on aarch64 rather than a warning. */
int mp3_decode_buffer(const void *data, size_t size,
                      int16_t **outPcm, size_t *outFrames,
                      int *outRate, int *outChannels);

#endif
