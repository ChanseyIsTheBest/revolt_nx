/* pps_perf.h -- frame/lock profiling, compiled out by default. MIT licensed. */
#ifndef RVNX_PPS_PERF_H
#define RVNX_PPS_PERF_H

#include <stdint.h>

enum { PERF_MUTEX = 0, PERF_COND, PERF_AUDIO_CB, PERF_IO, PERF_CAT_COUNT };

#define PERF_SPIKE_MS 25.0

#define perf_event(n)              ((void)(n))
#define perf_begin()               ((uint64_t)0)
#define perf_end(c, t, d)          ((void)(c), (void)(t), (void)(d))
#define perf_count(c)              ((void)(c))
#define perf_audio_underrun()      ((void)0)
#define perf_mark_main_thread()    ((void)0)
#define perf_frame_begin()         ((void)0)
#define perf_frame_end(s, w)       ((void)(s), (void)(w))

#endif
