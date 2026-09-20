/* rv_shim.h -- the bionic-vs-newlib odds and ends imports.c needs.
 *
 * These were pulled in by the import-table generator's --ref pass, which
 * reuses expressions a working port already proved correct. That was the right
 * instinct and the wrong mechanism: it copied names out of source files this
 * tree never adopted, so the table referenced 30 functions that existed
 * nowhere. They are re-implemented here with the same behaviour.
 *
 * Three groups, for three different reasons:
 *
 *   ctype    newlib's isalnum and friends are MACROS, so &isalnum does not
 *            compile. Real functions are needed to put in a table.
 *   ABI      clock ids, locale categories and sigset layouts differ between
 *            bionic and newlib, so these convert rather than forward.
 *   absent   newlib on Switch has no writev, sigsetjmp or working signals;
 *            these stand in.
 *
 * Names are prefixed rvnx_ wherever newlib might also declare the symbol, so
 * a toolchain that does ship it cannot collide with this one.
 *
 * MIT licensed.
 */
#ifndef RVNX_RV_SHIM_H
#define RVNX_RV_SHIM_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* --- ctype, as functions ---------------------------------------------- */
#include <wchar.h>    /* wint_t: bionic's wide ctype takes and returns it */

int isalnum_fn(int c);
int isalpha_fn(int c);
int isprint_fn(int c);
int isspace_fn(int c);
int iswcntrl_fn(wint_t c);
int tolower_fn(int c);
int toupper_fn(int c);
wint_t towlower_fn(wint_t c);
wint_t towupper_fn(wint_t c);

/* --- time ------------------------------------------------------------- */
int         clock_gettime_bionic(int androidClockId, struct timespec *tp);
time_t      pps_time(time_t *tp);
struct tm  *pps_localtime(const time_t *tp);
int         pps_nanosleep(const struct timespec *request, struct timespec *remain);
int         pps_gettimeofday(void *tv, void *tz);

/* --- filesystem ------------------------------------------------------- */
int   pps_fsync(int fd);
int   pps_ftruncate(int fd, long length);
void *pps_opendir(const char *path);
long  pps_readlink(const char *path, char *buffer, size_t size);

/* --- stdio ------------------------------------------------------------ */
int pps_printf_discard(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int pps_puts_discard(const char *s);

/* --- signals ---------------------------------------------------------- */
int raise_stub(int sig);
int sigaction_stub(int sig, const void *act, void *old);
int sigemptyset_stub(void *set);

/* --- setjmp ----------------------------------------------------------- */
int  pps_sigsetjmp(void *env, int saveMask);
void pps_siglongjmp(void *env, int value) __attribute__((noreturn));

/* --- locale ----------------------------------------------------------- */
char *setlocale_bionic(int category, const char *locale);

/* --- newlib macros, wrapped ------------------------------------------
 *
 * newlib defines several of these as macros for speed, and &putc does not
 * compile. Wrapping costs one call and is the only way to put them in a table.
 * Harmless if a given toolchain happens to provide real functions too. */
int  rvnx_putc(int c, void *stream);
int  rvnx_putchar(int c);
int  rvnx_getc_unlocked(void *stream);
int  rvnx_clearerr_fn(void *stream);
int  rvnx_isnanf(float value);

/* --- absent from newlib ----------------------------------------------- */
int  rvnx_isfinitef(float value);
long rvnx_writev(int fd, const void *iov, int count);
int  rvnx_pthread_setschedparam(unsigned long thread, int policy, const void *param);

/* Declared by devkitA64's <pthread.h> but not implemented anywhere in newlib
 * or libnx, so a table entry pointing at it compiles and then fails at the
 * link. Thread naming is a GNU extension and purely cosmetic here. */
int  rvnx_pthread_setname_np(unsigned long thread, const char *name);

/* Same shape: declared in <unistd.h>, no implementation, and nothing on this
 * filesystem supports hard links anyway. */
int  rvnx_link(const char *from, const char *to);

/* bionic reads its stack canary from TPIDR_EL0+0x28 (see install_bionic_tls),
 * not from this object, but the import must still resolve to something with a
 * stable address. Deliberately not named __stack_chk_guard: the toolchain has
 * its own. */
extern uintptr_t rvnx_stack_chk_guard;

/* libc_shim.c defines this and notes that imports.c references it, but no
 * header ever declared it. bionic's __sF is the array behind stdin, stdout and
 * stderr; the shim hands the module three 0x100-byte slots and recognises a
 * pointer into them. */
extern unsigned char fake_sF[3][0x100];

#endif
