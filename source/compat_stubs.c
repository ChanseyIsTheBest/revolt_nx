/* compat_stubs.c -- see compat_stubs.h. MIT licensed. */

#include <switch.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>

#include "compat_stubs.h"
#include "config.h"
#include "imports.h"
#include "rv_net.h"
#include "rv_log.h"
#include "rv_paths.h"

/* --- mmap arena ------------------------------------------------------- */

void  *g_mmap_arena_base = NULL;
size_t g_mmap_arena_size = 0;

/* Must be non-zero: libc_shim divides by it. 2 MB matches the granularity
 * bionic's allocator assumes for large mappings. */
size_t g_mmap_big_align = 2 * 1024 * 1024;

/* RVGL's allocation pattern is modest and known at build time, so refusing an
 * over-large request is better than handing back memory we cannot back. A
 * failed mmap surfaces as a clean out-of-memory instead of a fault later. */
int g_overcommit = 0;

#define RVNX_MMAP_ARENA_SIZE (64u * 1024u * 1024u)

void pps_mmap_arena_init(void) {
    if (g_mmap_arena_base != NULL) return;

    void *base = aligned_alloc(g_mmap_big_align, RVNX_MMAP_ARENA_SIZE);
    if (base == NULL) {
        /* Not fatal on its own: libc_shim falls back to malloc for small
         * requests. Large ones will fail, and that is the honest answer. */
        return;
    }

    g_mmap_arena_base = base;
    g_mmap_arena_size = RVNX_MMAP_ARENA_SIZE;
}

/* --- dlsym backing ---------------------------------------------------- */

void *firebase_stub_lookup(const char *name) {
    (void)name;
    return NULL;  /* RVGL links no analytics SDK. */
}

/* --- BSD socket bridge --------------------------------------------------
 *
 * These names predate rv_net.c and are still referenced by the copied
 * libc_shim/imports_helpers, so they stay as thin forwards. There is exactly
 * one implementation of each translation, in rv_net.c; having two would let
 * them drift, and a divergence here is silent. */

int  nx_accept(int fd, void *a, void *l)        { return rvn_accept(fd, a, l); }
int  nx_bind(int fd, const void *a, unsigned l) { return rvn_bind(fd, a, l); }
int  nx_connect(int fd, const void *a, unsigned l){ return rvn_connect(fd, a, l); }
void nx_freeaddrinfo(void *res)                 { rvn_freeaddrinfo(res); }
int  nx_getaddrinfo(const char *n, const char *s, const void *h, void **r)
{ return rvn_getaddrinfo(n, s, h, r); }
int  nx_getnameinfo(const void *a, unsigned al, char *h, unsigned hl,
                    char *s, unsigned sl, int f)
{ return rvn_getnameinfo(a, al, h, hl, s, sl, f); }
int  nx_getpeername(int fd, void *a, void *l)   { return rvn_getpeername(fd, a, l); }
int  nx_getsockname(int fd, void *a, void *l)   { return rvn_getsockname(fd, a, l); }
int  nx_getsockopt(int fd, int lv, int n, void *v, void *l)
{ return rvn_getsockopt(fd, lv, n, v, l); }
int  nx_setsockopt(int fd, int lv, int n, const void *v, unsigned l)
{ return rvn_setsockopt(fd, lv, n, v, l); }
int  nx_listen(int fd, int backlog)             { return rvn_listen(fd, backlog); }
int  nx_socket(int d, int t, int p)             { return rvn_socket(d, t, p); }
int  nx_shutdown(int fd, int how)               { return rvn_shutdown(fd, how); }
int  nx_select(int n, void *r, void *w, void *e, void *t)
{ return rvn_select(n, r, w, e, t); }
int  nx_poll(void *fds, unsigned long nfds, int timeout)
{ return rvn_poll(fds, nfds, timeout); }

/* RVGL imports none of these four -- ENet does all I/O through sendmsg and
 * recvmsg -- but libc_shim declares them, so they resolve rather than link
 * against nothing. */
long nx_recv(int fd, void *b, size_t l, int f)  { (void)fd;(void)b;(void)l;(void)f; return -1; }
long nx_recvfrom(int fd, void *b, size_t l, int f, void *a, void *al)
{ (void)fd;(void)b;(void)l;(void)f;(void)a;(void)al; return -1; }
long nx_send(int fd, const void *b, size_t l, int f) { (void)fd;(void)b;(void)l;(void)f; return -1; }
long nx_sendto(int fd, const void *b, size_t l, int f, const void *a, unsigned al)
{ (void)fd;(void)b;(void)l;(void)f;(void)a;(void)al; return -1; }

/* --- paths and tracing ------------------------------------------------- */

const char *sj_home(void) {
    const char *data = rv_paths_data();
    /* Before rv_paths_resolve runs there is nothing sensible to return, and
     * handing back NULL would fault inside strlen() rather than anywhere
     * informative. */
    return (data != NULL && *data != '\0') ? data : "sdmc:/switch";
}

/* Was unconditional, and produced several thousand identical "[mark] fopen"
 * lines in the first log that reached the menu -- enough noise to bury the
 * handful of lines that mattered. The failed-open path reports separately and
 * always. */
void sj_mark(const char *what) {
#if defined(RVNX_TRACE_IO) && RVNX_TRACE_IO
    LOGI("[mark] %s", what != NULL ? what : "");
#else
    (void)what;
#endif
}

/* Successful opens are silent by default -- RVGL opens thousands of files
 * during a track load and logging them costs more than it tells you. Build
 * with RVNX_TRACE_IO=1 for those.
 *
 * FAILURES are always logged, bounded. A missing file is rare, it is almost
 * always the explanation for a black screen or a silent exit, and needing a
 * rebuild to see it is exactly the wrong trade. */
void sj_trace_open(const char *path, int ok) {
    if (!ok) {
        static int reported;
        if (reported < 40) {
            ++reported;
            LOGW("[open] MISS %s", path != NULL ? path : "?");
        }
        return;
    }
#if defined(RVNX_TRACE_IO) && RVNX_TRACE_IO
    LOGI("[open] ok   %s", path != NULL ? path : "?");
#else
    (void)path;
#endif
}
