/* rv_net.h -- bionic-ABI BSD sockets over libnx, for RVGL's multiplayer.
 *
 * RVGL statically links ENet, so the socket calls appear directly in
 * libmain.so's import table rather than coming from a separate library. The
 * exact set it imports, read from the binary:
 *
 *     socket  bind  connect  listen  accept  shutdown  close
 *     setsockopt  getsockopt  getsockname
 *     getaddrinfo  freeaddrinfo  getnameinfo  inet_ntop  inet_pton
 *     select  poll  fcntl  ioctl
 *     sendmsg  recvmsg
 *
 * Note what is ABSENT: send, recv, sendto and recvfrom. ENet 1.3.14+ does all
 * of its I/O through sendmsg/recvmsg with scatter/gather iovecs, so those two
 * are the entire data path. A stub returning 0 there reads as a clean EOF and
 * multiplayer fails silently with no error anywhere -- which is exactly what
 * the previous passthrough bridge would have done, since it had neither.
 *
 * WHY A TRANSLATION LAYER IS NEEDED AT ALL
 * ----------------------------------------
 * libmain.so was compiled against bionic; libnx's sockets are BSD-derived.
 * The two disagree about nearly every constant and several struct layouts:
 *
 *   sockaddr_in   bionic {u16 family; u16 port; u32 addr; u8 zero[8]}
 *                 BSD has a leading sin_len byte
 *   SOL_SOCKET    1 vs 0xffff
 *   SO_*          every value differs (SO_BROADCAST 6 vs 0x20, and so on)
 *   MSG_DONTWAIT  0x40 vs 0x80
 *   O_NONBLOCK    0x800 vs newlib's value
 *   FIONBIO       0x5421 vs BSD's _IOW-encoded request
 *   msghdr        different field offsets and padding
 *   addrinfo      ai_addrlen width differs
 *   errno         EINPROGRESS 115 vs 119, and friends
 *
 * Passing a bionic constant straight through does not fault. setsockopt
 * returns an error the engine ignores, or worse, silently sets a *different*
 * option. That is why this is a table rather than a forward.
 *
 * MIT licensed.
 */
#ifndef RVNX_RV_NET_H
#define RVNX_RV_NET_H

#include <stddef.h>
#include <stdint.h>

/* Brings up the BSD service sized for ENet, plus nifm. Safe to call twice. */
void rv_net_init(void);
void rv_net_exit(void);

/* nifm connection state, cached for a second so a per-frame call is free. */
int  rv_net_online(void);

/* One line for the log: service state, session count, link status. */
void rv_net_summarise(char *buffer, size_t capacity);

/* The console's LAN address, for telling other players where to connect.
 * "unknown" when nifm cannot say. */
void rv_net_local_address(char *buffer, size_t capacity);

/* Datagram and error counts. A netgame that does not work is otherwise
 * completely silent about why. */
void rv_net_traffic(char *buffer, size_t capacity);

/* ---- the bionic-ABI entry points, wired in imports.c ------------------- */

int  rvn_socket(int domain, int type, int protocol);
int  rvn_bind(int fd, const void *addr, unsigned addrlen);
int  rvn_connect(int fd, const void *addr, unsigned addrlen);
int  rvn_listen(int fd, int backlog);
int  rvn_accept(int fd, void *addr, void *addrlen);
int  rvn_shutdown(int fd, int how);
int  rvn_close(int fd);

int  rvn_setsockopt(int fd, int level, int name, const void *value, unsigned len);
int  rvn_getsockopt(int fd, int level, int name, void *value, void *len);
int  rvn_getsockname(int fd, void *addr, void *addrlen);
int  rvn_getpeername(int fd, void *addr, void *addrlen);

long rvn_sendmsg(int fd, const void *msg, int flags);
long rvn_recvmsg(int fd, void *msg, int flags);

int  rvn_getaddrinfo(const char *node, const char *service, const void *hints, void **res);
void rvn_freeaddrinfo(void *res);
int  rvn_getnameinfo(const void *addr, unsigned addrlen, char *host, unsigned hostlen,
                     char *serv, unsigned servlen, int flags);

int  rvn_select(int nfds, void *readfds, void *writefds, void *exceptfds, void *timeout);
int  rvn_poll(void *fds, unsigned long nfds, int timeout);
int  rvn_fcntl(int fd, int cmd, ...);
int  rvn_ioctl(int fd, unsigned long request, ...);

/* Called from close_fake() so a reused fd number is not misclassified as a
 * socket by a later call. */
void rvn_untrack(int fd);
int  rvn_close_hook(int fd);   /* imports.c points `close` here */
int  rvn_is_socket(int fd);

#endif
