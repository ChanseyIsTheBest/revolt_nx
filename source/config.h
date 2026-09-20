/* config.h -- build-wide constants for revoltnx. MIT licensed. */
#ifndef RVNX_CONFIG_H
#define RVNX_CONFIG_H

/* LOGB below expands to log_write, so anything including config.h must see
 * the logging API. Pulling it in here means a reused file that includes only
 * config.h -- as several do -- still compiles. */
#include "rv_log.h"

/* The port locates itself from argv[0] at startup -- see rv_paths.h -- so
 * there is no fixed install path. This name survives only as the first entry
 * in rv_paths.c's fallback list, for launchers that supply no argv[0].
 *
 * RVGL resolves its data files against the working directory, so main.c
 * chdir()s to the resolved data directory before calling SDL_main. */
#define RVNX_LEGACY_ROOT "sdmc:/switch/revoltnx"

/* Floors for each module's LOAD reservation.
 *
 * The real size is computed at load time from the file's program headers
 * (RequiredLoadSize in main.c), so these are minimums and headroom, not the
 * answer. That matters because a module's memory image and its file size are
 * not related:
 *
 *   module            file      memory image   of which .bss
 *   libmain.so         2.9 MB      170.8 MiB      167.9 MiB
 *   libopenal.so       0.9 MB        0.9 MiB
 *   libmpg123.so       0.3 MB        0.4 MiB
 *   libsndfile.so      0.5 MB        0.5 MiB
 *   libunistring.so    1.8 MB        1.7 MiB
 *
 * libmain.so is a 2.9 MB file that needs 170 MiB of address space. Sizing the
 * reservation from the file, which this once did, is short by a factor of
 * fifty and so_load fails with nothing to say why.
 *
 * This must run as a title override -- hold R while starting an installed
 * game. Applet mode does not have the heap for ~280 MiB of modules plus the
 * engine's own allocations. */
#define RVNX_MAIN_LOAD_SIZE      (256 * 1024 * 1024)
#define RVNX_OPENAL_LOAD_SIZE    (  8 * 1024 * 1024)
#define RVNX_MPG123_LOAD_SIZE    (  4 * 1024 * 1024)
#define RVNX_SNDFILE_LOAD_SIZE   (  4 * 1024 * 1024)
#define RVNX_UNISTRING_LOAD_SIZE (  8 * 1024 * 1024)

/* Added on top of the computed image before rounding to a 64 KiB boundary. */
#define RVNX_LOAD_SLACK          (  1 * 1024 * 1024)


/* ------------------------------------------------------------------ */
/* Knobs the reused reference files expect                             */
/* ------------------------------------------------------------------ */
/*
 * libc_shim.c, imports_helpers.c and opensles.c are compiled in unmodified
 * from another Switch port, and they read a handful of settings that lived in
 * that port's config.h. Replacing that header rather than copying it dropped
 * them, which shows up as an implicit-declaration error at build time -- or,
 * for the struct, a link error. They are re-provided here with the same
 * meanings and conservative defaults.
 */

/* Verbose boot logging. Off: the [I] lines are a firehose and the SD card is
 * slow. Warnings and errors are unaffected. */
#ifndef RVNX_DEBUG_LOG
#define RVNX_DEBUG_LOG 0
#endif

#if RVNX_DEBUG_LOG
#define LOGB(...) log_write('I', __VA_ARGS__)
#else
#define LOGB(...) ((void)0)
#endif

/* Condition-variable contention tracing in imports_helpers.c. Off: it logs on
 * every wait, which during a race is thousands of lines a second. */
#ifndef PPS_COND_DIAG
#define PPS_COND_DIAG 0
#endif

/* Compatibility switches read at runtime by the reused files.
 *
 * decode_stream_audio gates OpenSL's MIME / file-descriptor source path, which
 * decodes a compressed stream rather than taking PCM from a buffer queue. RVGL
 * feeds the buffer queue -- it carries its own Vorbis decoder and links
 * libmpg123 for the rest -- so the path should not be reached. Left enabled
 * because being wrong in that direction costs a log line, and being wrong in
 * the other costs silence with no explanation. */
typedef struct {
    int decode_stream_audio;
} RvCompatConfig;

extern RvCompatConfig config;

#endif
