/* rv_log.h -- Android log API over our file log, plus two process shims.
 * MIT licensed. */
#ifndef RVNX_RV_LOG_H
#define RVNX_RV_LOG_H

#include <stdarg.h>

int  rvnx_log_print(int prio, const char *tag, const char *fmt, ...);
int  rvnx_log_vprint(int prio, const char *tag, const char *fmt, va_list args);
int  rvnx_log_write_prio(int prio, const char *tag, const char *text);
void rvnx_log_assert(const char *cond, const char *tag, const char *fmt, ...);
void rvnx_log_write(const char *text);

/* Referenced by the import table for bionic-only no-ops. */
void rvnx_noop(void);

/* The macro set the copied reference files use. They lived in that port's
 * config.h, which this tree replaced, so they are re-provided here on top of
 * rvnx_log_print rather than editing files that are otherwise unmodified.
 *
 * The level numbers are Android's: 4 INFO, 5 WARN, 6 ERROR. */
void log_write(char level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void log_close(void);
void log_flush(void);

#define LOGI(...) log_write('I', __VA_ARGS__)
#define LOGW(...) log_write('W', __VA_ARGS__)
#define LOGE(...) log_write('E', __VA_ARGS__)

/* exit() replacement. Newlib's would tear down the process from inside a
 * module we still own, so the engine's request is turned into a flag the
 * loader acts on after SDL_main returns. */
void rvnx_exit(int code) __attribute__((noreturn));

#endif
