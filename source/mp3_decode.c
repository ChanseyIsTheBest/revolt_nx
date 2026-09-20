/* mp3_decode.c -- stub. See mp3_decode.h. MIT licensed. */
#include "mp3_decode.h"

int mp3_decode_buffer(const void *data, size_t size,
                      int16_t **outPcm, size_t *outFrames,
                      int *outRate, int *outChannels) {
    (void)data; (void)size;
    if (outPcm != NULL) *outPcm = NULL;
    if (outFrames != NULL) *outFrames = 0;
    if (outRate != NULL) *outRate = 0;
    if (outChannels != NULL) *outChannels = 0;
    return 0;  /* RVGL decodes its own audio through libmpg123.so. */
}
