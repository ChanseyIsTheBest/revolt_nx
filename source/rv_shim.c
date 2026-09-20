/* rv_shim.c -- see rv_shim.h. MIT licensed. */

#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <locale.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <wctype.h>

#include "rv_shim.h"
#include "rv_log.h"

/* --- ctype, as functions ----------------------------------------------
 *
 * Wrapping in parentheses suppresses the macro, so these call newlib's real
 * functions rather than recursing. */
int isalnum_fn(int c)  { return (isalnum)(c); }
int isalpha_fn(int c)  { return (isalpha)(c); }
int isprint_fn(int c)  { return (isprint)(c); }
int isspace_fn(int c)  { return (isspace)(c); }
int iswcntrl_fn(wint_t c) { return (iswcntrl)(c); }
int tolower_fn(int c)  { return (tolower)(c); }
int toupper_fn(int c)  { return (toupper)(c); }
wint_t towlower_fn(wint_t c) { return (towlower)(c); }
wint_t towupper_fn(wint_t c) { return (towupper)(c); }

/* --- time -------------------------------------------------------------- */

int clock_gettime_bionic(int androidClockId, struct timespec *tp) {
    if (tp == NULL) { errno = EINVAL; return -1; }

    /* Android's clock ids are not newlib's. Passing one straight through gets
     * CLOCK_MONOTONIC when the caller asked for CLOCK_REALTIME, which shows up
     * as a clock that never advances between runs. */
    clockid_t host;
    switch (androidClockId) {
        case 0:  /* CLOCK_REALTIME         */
        case 5:  /* CLOCK_REALTIME_COARSE  */
            host = CLOCK_REALTIME;
            break;
        case 1:  /* CLOCK_MONOTONIC        */
        case 2:  /* CLOCK_PROCESS_CPUTIME_ID: elapsed monotonic is close enough */
        case 3:  /* CLOCK_THREAD_CPUTIME_ID */
        case 4:  /* CLOCK_MONOTONIC_RAW    */
        case 6:  /* CLOCK_MONOTONIC_COARSE */
        case 7:  /* CLOCK_BOOTTIME         */
            host = CLOCK_MONOTONIC;
            break;
        default:
            errno = EINVAL;
            return -1;
    }

    return clock_gettime(host, tp);
}

time_t     pps_time(time_t *tp)              { return time(tp); }
struct tm *pps_localtime(const time_t *tp)   { return localtime(tp); }

int pps_nanosleep(const struct timespec *request, struct timespec *remain) {
    return nanosleep(request, remain);
}

int pps_gettimeofday(void *tv, void *tz) {
    return gettimeofday((struct timeval *)tv, (struct timezone *)tz);
}

/* --- filesystem -------------------------------------------------------- */

/* The SD card has no write barrier we can drive, and reporting failure would
 * make the engine treat a perfectly good save as lost. */
int pps_fsync(int fd) { (void)fd; return 0; }

int pps_ftruncate(int fd, long length) { (void)fd; (void)length; return 0; }

void *pps_opendir(const char *path) { return opendir(path); }

/* Nothing on this filesystem is a symlink, and EINVAL is exactly what
 * readlink reports for a path that is not one. */
long pps_readlink(const char *path, char *buffer, size_t size) {
    (void)path; (void)buffer; (void)size;
    errno = EINVAL;
    return -1;
}

/* --- stdio ------------------------------------------------------------- */

/* The module's printf goes to our log rather than to a console that may not
 * exist by then. Writing to stdout after consoleExit draws into a framebuffer
 * the console no longer owns. */
int pps_printf_discard(const char *fmt, ...) {
    char line[1024];
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
    rvnx_log_write(line);
    return n;
}

int pps_puts_discard(const char *s) {
    if (s != NULL) { rvnx_log_write(s); rvnx_log_write("\n"); }
    return 0;
}

/* --- signals ----------------------------------------------------------- */

/* Not routed to newlib's raise(). The engine calls it to abort after its own
 * handler has written a report, and newlib's would take the process down
 * through a path libnx cannot describe. Whatever caused it is already in the
 * log by this point, so say so and continue. */
int raise_stub(int sig) {
    rvnx_log_print(6, "shim", "engine raised signal %d; continuing", sig);
    return 0;
}

/* The buffer belongs to the MODULE, so it is bionic-sized, not newlib-sized.
 * Zeroing sizeof(struct sigaction) would clear 24 of the 32 bytes the caller
 * allocated and leave the rest as whatever was on its stack -- and on a
 * toolchain where the host struct is the larger of the two it would overrun.
 * bionic's arm64 layout is fixed, so use its sizes explicitly; that also keeps
 * this file from needing <signal.h>, which newlib does not fully populate.
 *
 *   struct sigaction  int sa_flags; pad; handler; sigset_t; restorer  = 32
 *   sigset_t          unsigned long                                   =  8
 */
#define BIONIC_SIGACTION_BYTES 32
#define BIONIC_SIGSET_BYTES     8

int sigaction_stub(int sig, const void *act, void *old) {
    (void)sig; (void)act;
    /* A zeroed old handler means "there was no previous one", which is both
     * true here and a case the engine's own code already handles. */
    if (old != NULL) memset(old, 0, BIONIC_SIGACTION_BYTES);
    return 0;
}

int sigemptyset_stub(void *set) {
    if (set != NULL) memset(set, 0, BIONIC_SIGSET_BYTES);
    return 0;
}

/* --- setjmp ------------------------------------------------------------ */

/* newlib has no sigsetjmp. There is no signal mask to save either, so the
 * plain pair is exactly equivalent here. */
int pps_sigsetjmp(void *env, int saveMask) {
    (void)saveMask;
    return setjmp(*(jmp_buf *)env);
}

void pps_siglongjmp(void *env, int value) {
    longjmp(*(jmp_buf *)env, value ? value : 1);
}

/* --- locale ------------------------------------------------------------ */

static int LocaleCategoryToHost(int bionicCategory) {
    /* bionic: CTYPE 0, NUMERIC 1, TIME 2, COLLATE 3, MONETARY 4, MESSAGES 5,
     * ALL 6.  newlib: ALL 0, COLLATE 1, CTYPE 2, MONETARY 3, NUMERIC 4,
     * TIME 5, MESSAGES 6.  Nothing lines up. */
    switch (bionicCategory) {
        case 0: return LC_CTYPE;
        case 1: return LC_NUMERIC;
        case 2: return LC_TIME;
        case 3: return LC_COLLATE;
        case 4: return LC_MONETARY;
#ifdef LC_MESSAGES
        case 5: return LC_MESSAGES;
#endif
        case 6: return LC_ALL;
        default: return LC_ALL;
    }
}

char *setlocale_bionic(int category, const char *locale) {
    return setlocale(LocaleCategoryToHost(category), locale);
}

/* --- absent from newlib ------------------------------------------------ */

int rvnx_isfinitef(float value) { return isfinite(value) ? 1 : 0; }
int rvnx_isnanf(float value)    { return isnan(value) ? 1 : 0; }

int rvnx_putc(int c, void *stream)      { return (putc)(c, (FILE *)stream); }
int rvnx_putchar(int c)                 { return (putchar)(c); }
int rvnx_getc_unlocked(void *stream)    { return (getc)((FILE *)stream); }
int rvnx_clearerr_fn(void *stream)      { (clearerr)((FILE *)stream); return 0; }

/* Matching the layout rather than including <sys/uio.h>, which devkitA64 does
 * not reliably ship. */
struct RvnxIovec { void *base; size_t length; };

long rvnx_writev(int fd, const void *iov, int count) {
    if (iov == NULL || count < 0) { errno = EINVAL; return -1; }

    const struct RvnxIovec *vectors = (const struct RvnxIovec *)iov;
    long total = 0;

    for (int i = 0; i < count; ++i) {
        if (vectors[i].length == 0) continue;

        const ssize_t written = write(fd, vectors[i].base, vectors[i].length);
        if (written < 0) return total > 0 ? total : -1;

        total += (long)written;
        /* A short write ends the call, as writev does. */
        if ((size_t)written != vectors[i].length) break;
    }

    return total;
}

/* Thread priority is set through libnx, not pthreads, and the engine treats a
 * failure here as fatal for the thread. Reporting success is the honest
 * outcome: the thread runs, just at the default priority. */
int rvnx_pthread_setschedparam(unsigned long thread, int policy, const void *param) {
    (void)thread; (void)policy; (void)param;
    return 0;
}

/* libnx names threads through its own API, not pthreads. The engine uses this
 * for its audio and network threads and ignores the result; reporting success
 * is accurate in the sense that matters -- the thread runs. */
int rvnx_pthread_setname_np(unsigned long thread, const char *name) {
    (void)thread; (void)name;
    return 0;
}

int rvnx_link(const char *from, const char *to) {
    (void)from; (void)to;
    errno = EPERM;   /* what a filesystem without hard links reports */
    return -1;
}

uintptr_t rvnx_stack_chk_guard = 0xDEADC0DEDEADC0DEULL;
