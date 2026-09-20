/* compat_stubs.h -- the pieces bionic expects that newlib does not provide.
 *
 * Three unrelated groups share this header because libc_shim.c and
 * imports_helpers.c are compiled in unmodified and include it by name:
 *
 *   1. an mmap arena. bionic's mmap backs large anonymous allocations; libnx
 *      has no mmap, so libc_shim carves them out of a reserved arena.
 *   2. a BSD socket bridge. RVGL statically links ENet and calls the socket
 *      API directly (accept/bind/connect/getaddrinfo are all in libmain.so's
 *      import list). libnx's sockets work but the struct and flag values need
 *      translating, which is what the nx_* wrappers do.
 *   3. dlsym() backing, so a module can resolve a symbol by name at runtime.
 *
 * MIT licensed.
 */
#ifndef RVNX_COMPAT_STUBS_H
#define RVNX_COMPAT_STUBS_H

#include <stdint.h>
#include <stddef.h>

/* --- mmap arena ------------------------------------------------------- */
extern void  *g_mmap_arena_base;   /* granule-aligned arena, or NULL          */
extern size_t g_mmap_arena_size;
extern size_t g_mmap_big_align;    /* NON-ZERO power of two; used as a divisor */
extern int    g_overcommit;        /* 0 = do not pretend allocations succeed  */

void pps_mmap_arena_init(void);

/* --- dlsym backing ---------------------------------------------------- */
uintptr_t dynlib_find_export(const char *name);

/* Analytics SDKs the reference ports had to answer for. RVGL links none, so
 * this always returns NULL; kept because imports_helpers.c calls it. */
void *firebase_stub_lookup(const char *name);

/* --- BSD socket bridge ------------------------------------------------ */
int  nx_accept(int fd, void *addr, void *addrlen);
int  nx_bind(int fd, const void *addr, unsigned addrlen);
int  nx_connect(int fd, const void *addr, unsigned addrlen);
void nx_freeaddrinfo(void *res);
int  nx_getaddrinfo(const char *node, const char *service, const void *hints, void **res);
int  nx_getnameinfo(const void *addr, unsigned addrlen, char *host, unsigned hostlen,
                    char *serv, unsigned servlen, int flags);
int  nx_getpeername(int fd, void *addr, void *addrlen);
int  nx_getsockname(int fd, void *addr, void *addrlen);
int  nx_getsockopt(int fd, int level, int name, void *val, void *len);
int  nx_setsockopt(int fd, int level, int name, const void *val, unsigned len);
int  nx_listen(int fd, int backlog);
long nx_recv(int fd, void *buf, size_t len, int flags);
long nx_recvfrom(int fd, void *buf, size_t len, int flags, void *addr, void *addrlen);
long nx_send(int fd, const void *buf, size_t len, int flags);
long nx_sendto(int fd, const void *buf, size_t len, int flags, const void *addr, unsigned addrlen);
int  nx_socket(int domain, int type, int protocol);
int  nx_shutdown(int fd, int how);
int  nx_select(int n, void *r, void *w, void *e, void *timeout);
int  nx_poll(void *fds, unsigned long nfds, int timeout);


/* --- paths and tracing, for the reused libc_shim ----------------------
 *
 * libc_shim.c calls these by name. sj_home() is the writable root it measures
 * paths against: it decides what counts as a "managed" path worth creating
 * parents for, and backs HOME and TMPDIR. Pointing it at the data directory
 * keeps every write the engine makes inside the RVGL folder, which is also
 * where SDL_AndroidGetExternalStoragePath reports, so the two agree. */
const char *sj_home(void);
void sj_mark(const char *what);
void sj_trace_open(const char *path, int ok);

#endif
