/* rv_net.c -- see rv_net.h. MIT licensed.
 *
 * Structure: constant tables first, then struct converters, then the entry
 * points. Every translation is a lookup rather than a cast, because the
 * failure mode of a wrong cast here is silent (an option that sets something
 * else, a recv that reports EOF) rather than a crash.
 */

/* struct addrinfo and the AI_* flags are POSIX, which strict -std=cNN hides.
 * libc_shim.c does the same for the same reason. */
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "rv_net.h"
#include "rv_log.h"

/* Bounded so a failing call inside ENet's service loop cannot flood the log:
 * enet_host_service runs every frame, so an unbounded trace here fills the SD
 * card faster than it tells you anything. */
static unsigned g_traces;
#define RVN_TRACE_LIMIT 32

static void Trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void Trace(const char *fmt, ...) {
    if (__atomic_fetch_add(&g_traces, 1, __ATOMIC_RELAXED) >= RVN_TRACE_LIMIT) return;
    char line[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
    rvnx_log_print(5, "net", "%s", line);
}

static void SleepMs(int ms) {
    if (ms <= 0) return;
#ifdef __SWITCH__
    svcSleepThread((u64)ms * 1000000ULL);
#else
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

static int ElapsedMs(uint64_t startTick) {
#ifdef __SWITCH__
    return (int)(armTicksToNs(armGetSystemTick() - startTick) / 1000000ULL);
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    const uint64_t ns = (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
    return (int)((ns - startTick) / 1000000ULL);
#endif
}

static uint64_t NowTick(void) {
#ifdef __SWITCH__
    return armGetSystemTick();
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
#endif
}

/* ------------------------------------------------------------------ */
/* bionic constants                                                    */
/* ------------------------------------------------------------------ */

#define B_AF_INET        2
#define B_AF_INET6       10      /* BSD uses 28; we do not translate IPv6 */

#define B_SOL_SOCKET     1
#define B_IPPROTO_IP     0
#define B_IPPROTO_TCP    6
#define B_IPPROTO_UDP    17

#define B_SO_DEBUG       1
#define B_SO_REUSEADDR   2
#define B_SO_TYPE        3
#define B_SO_ERROR       4
#define B_SO_DONTROUTE   5
#define B_SO_BROADCAST   6
#define B_SO_SNDBUF      7
#define B_SO_RCVBUF      8
#define B_SO_KEEPALIVE   9
#define B_SO_OOBINLINE   10
#define B_SO_LINGER      13
#define B_SO_REUSEPORT   15
#define B_SO_RCVLOWAT    18
#define B_SO_SNDLOWAT    19
#define B_SO_RCVTIMEO    20
#define B_SO_SNDTIMEO    21

#define B_MSG_OOB        0x01
#define B_MSG_PEEK       0x02
#define B_MSG_DONTROUTE  0x04
#define B_MSG_TRUNC      0x20
#define B_MSG_DONTWAIT   0x40
#define B_MSG_WAITALL    0x100

#define B_O_NONBLOCK     0x800
/* bionic lets SOCK_NONBLOCK ride in socket()'s type argument; same bit. */
#define B_SOCK_NONBLOCK_MASK 0x800
#define B_F_GETFL        3
#define B_F_SETFL        4
#define B_F_GETFD        1
#define B_F_SETFD        2

#define B_FIONREAD       0x541B
#define B_FIONBIO        0x5421

#define B_POLLIN         0x001
#define B_POLLPRI        0x002
#define B_POLLOUT        0x004
#define B_POLLERR        0x008
#define B_POLLHUP        0x010
#define B_POLLNVAL       0x020
#define B_POLLRDNORM     0x040
#define B_POLLRDBAND     0x080
#define B_POLLWRNORM     0x100
#define B_POLLWRBAND     0x200

/* Linux errno numbers the engine compares against. */
#define LX_EAGAIN        11
#define LX_EINPROGRESS   115
#define LX_EALREADY      114
#define LX_ENOTSOCK      88
#define LX_EADDRINUSE    98
#define LX_ECONNREFUSED  111
#define LX_ECONNRESET    104
#define LX_ETIMEDOUT     110
#define LX_EHOSTUNREACH  113
#define LX_ENETUNREACH   101
#define LX_EMSGSIZE      90
#define LX_ENOTTY        25
#define LX_ENOPROTOOPT   92
#define LX_EAFNOSUPPORT  97
#define LX_EINVAL        22

/* ------------------------------------------------------------------ */
/* bionic struct layouts                                               */
/* ------------------------------------------------------------------ */

struct BSockaddrIn {
    uint16_t family;
    uint16_t port;      /* network order, same on both sides */
    uint32_t addr;      /* network order */
    uint8_t  zero[8];
};

struct BIovec {
    void  *base;
    size_t len;
};

struct BMsghdr {
    void     *name;
    uint32_t  namelen;
    /* 4 bytes of padding here on arm64 */
    struct BIovec *iov;
    size_t    iovlen;
    void     *control;
    size_t    controllen;
    int       flags;
};

struct BAddrInfo {
    int       flags;
    int       family;
    int       socktype;
    int       protocol;
    uint32_t  addrlen;
    /* 4 bytes of padding */
    char     *canonname;
    struct BSockaddrIn *addr;
    struct BAddrInfo   *next;
};

struct BPollfd {
    int     fd;
    int16_t events;
    int16_t revents;
};

/* These offsets are the whole contract with the engine. If any assert fires,
 * every socket call is reading the wrong fields. */
_Static_assert(sizeof(struct BSockaddrIn) == 16, "bionic sockaddr_in is 16 bytes");
_Static_assert(offsetof(struct BMsghdr, iov) == 16, "bionic arm64 msghdr.iov");
_Static_assert(offsetof(struct BMsghdr, flags) == 48, "bionic arm64 msghdr.flags");
_Static_assert(offsetof(struct BAddrInfo, canonname) == 24, "bionic addrinfo.canonname");
_Static_assert(offsetof(struct BAddrInfo, next) == 40, "bionic addrinfo.next");
_Static_assert(sizeof(struct BPollfd) == 8, "bionic pollfd is 8 bytes");

/* ------------------------------------------------------------------ */
/* errno                                                               */
/* ------------------------------------------------------------------ */

/* Rewrites the host's errno into the Linux number the engine expects.
 *
 * EAGAIN and EWOULDBLOCK are 11 on both sides, which is the one that matters
 * most for ENet's non-blocking UDP reads. The rest differ, and EINPROGRESS in
 * particular (119 here, 115 there) would make a normal asynchronous connect
 * look like a hard failure. */
static int TranslateErrno(void) {
    const int e = errno;
    switch (e) {
#ifdef EAGAIN
        case EAGAIN:        return LX_EAGAIN;
#endif
#ifdef EINPROGRESS
        case EINPROGRESS:   return LX_EINPROGRESS;
#endif
#ifdef EALREADY
        case EALREADY:      return LX_EALREADY;
#endif
#ifdef ENOTSOCK
        case ENOTSOCK:      return LX_ENOTSOCK;
#endif
#ifdef EADDRINUSE
        case EADDRINUSE:    return LX_EADDRINUSE;
#endif
#ifdef ECONNREFUSED
        case ECONNREFUSED:  return LX_ECONNREFUSED;
#endif
#ifdef ECONNRESET
        case ECONNRESET:    return LX_ECONNRESET;
#endif
#ifdef ETIMEDOUT
        case ETIMEDOUT:     return LX_ETIMEDOUT;
#endif
#ifdef EHOSTUNREACH
        case EHOSTUNREACH:  return LX_EHOSTUNREACH;
#endif
#ifdef ENETUNREACH
        case ENETUNREACH:   return LX_ENETUNREACH;
#endif
#ifdef EMSGSIZE
        case EMSGSIZE:      return LX_EMSGSIZE;
#endif
        default: break;
    }
    return e;
}

/* Applies the translated value so the engine's next errno read is correct. */
static int Fail(int result) {
    errno = TranslateErrno();
    return result;
}

/* ------------------------------------------------------------------ */
/* Socket bookkeeping                                                  */
/* ------------------------------------------------------------------ */

#define RVN_MAXFD 1024

static uint8_t s_isSocket[RVN_MAXFD];
static uint8_t s_wantNonBlock[RVN_MAXFD];

int rvn_is_socket(int fd) {
    return fd >= 0 && fd < RVN_MAXFD && s_isSocket[fd];
}

void rvn_untrack(int fd) {
    if (fd >= 0 && fd < RVN_MAXFD) {
        s_isSocket[fd] = 0;
        s_wantNonBlock[fd] = 0;
    }
}

static void Track(int fd) {
    if (fd >= 0 && fd < RVN_MAXFD) s_isSocket[fd] = 1;
}

/* ------------------------------------------------------------------ */
/* Constant translation                                                */
/* ------------------------------------------------------------------ */

static int MapFamily(int bionicFamily) {
    switch (bionicFamily) {
        case 0:           return AF_UNSPEC;
        case B_AF_INET:   return AF_INET;
        case B_AF_INET6:  return AF_INET6;   /* 10 here, 28 on BSD */
        default: return -1;
    }
}

static int MapFamilyToGuest(int hostFamily) {
    if (hostFamily == AF_INET)  return B_AF_INET;
    if (hostFamily == AF_INET6) return B_AF_INET6;
    if (hostFamily == AF_UNSPEC) return 0;
    return hostFamily;
}

static int MapLevel(int bionicLevel) {
    switch (bionicLevel) {
        case B_SOL_SOCKET:  return SOL_SOCKET;
        case B_IPPROTO_IP:  return IPPROTO_IP;
        case B_IPPROTO_TCP: return IPPROTO_TCP;
        case B_IPPROTO_UDP: return IPPROTO_UDP;
        default: return -1;
    }
}

/* Socket-level options. Resolved to the host's own constants rather than
 * hardcoded BSD numbers, so a libnx header change cannot silently desync this.
 * Returns -1 for options we do not carry. */
static int MapSocketOption(int bionicOption) {
    switch (bionicOption) {
        case B_SO_DEBUG:      return SO_DEBUG;
        case B_SO_REUSEADDR:  return SO_REUSEADDR;
        case B_SO_TYPE:       return SO_TYPE;
        case B_SO_ERROR:      return SO_ERROR;
        case B_SO_DONTROUTE:  return SO_DONTROUTE;
        case B_SO_BROADCAST:  return SO_BROADCAST;
        case B_SO_SNDBUF:     return SO_SNDBUF;
        case B_SO_RCVBUF:     return SO_RCVBUF;
        case B_SO_KEEPALIVE:  return SO_KEEPALIVE;
        case B_SO_OOBINLINE:  return SO_OOBINLINE;
        case B_SO_LINGER:     return SO_LINGER;
#ifdef SO_REUSEPORT
        case B_SO_REUSEPORT:  return SO_REUSEPORT;
#endif
        case B_SO_RCVLOWAT:   return SO_RCVLOWAT;
        case B_SO_SNDLOWAT:   return SO_SNDLOWAT;
        case B_SO_RCVTIMEO:   return SO_RCVTIMEO;
        case B_SO_SNDTIMEO:   return SO_SNDTIMEO;
        default: return -1;
    }
}

static int MapSendFlags(int bionicFlags) {
    int out = 0;
    if (bionicFlags & B_MSG_OOB)       out |= MSG_OOB;
    if (bionicFlags & B_MSG_PEEK)      out |= MSG_PEEK;
    if (bionicFlags & B_MSG_DONTROUTE) out |= MSG_DONTROUTE;
#ifdef MSG_TRUNC
    if (bionicFlags & B_MSG_TRUNC)     out |= MSG_TRUNC;
#endif
#ifdef MSG_DONTWAIT
    if (bionicFlags & B_MSG_DONTWAIT)  out |= MSG_DONTWAIT;
#endif
#ifdef MSG_WAITALL
    if (bionicFlags & B_MSG_WAITALL)   out |= MSG_WAITALL;
#endif
    return out;
}

static int16_t MapPollEventsIn(int16_t bionicEvents) {
    int16_t out = 0;
    if (bionicEvents & (B_POLLIN | B_POLLRDNORM | B_POLLRDBAND)) out |= POLLIN;
    if (bionicEvents & (B_POLLOUT | B_POLLWRNORM | B_POLLWRBAND)) out |= POLLOUT;
    if (bionicEvents & B_POLLPRI) out |= POLLPRI;
    return out;
}

static int16_t MapPollEventsOut(int16_t hostRevents) {
    int16_t out = 0;
    if (hostRevents & POLLIN)   out |= (int16_t)(B_POLLIN | B_POLLRDNORM);
    if (hostRevents & POLLOUT)  out |= (int16_t)(B_POLLOUT | B_POLLWRNORM);
    if (hostRevents & POLLPRI)  out |= B_POLLPRI;
    if (hostRevents & POLLERR)  out |= B_POLLERR;
    if (hostRevents & POLLHUP)  out |= B_POLLHUP;
    if (hostRevents & POLLNVAL) out |= B_POLLNVAL;
    return out;
}

/* ------------------------------------------------------------------ */
/* Address conversion                                                  */
/* ------------------------------------------------------------------ */

/* Linux and BSD sockaddrs differ in exactly their first two bytes: Linux
 * keeps a 16-bit family there, BSD keeps a length byte then an 8-bit family.
 * Everything after that -- port, address, scope -- is laid out identically.
 *
 * So rather than convert struct by struct, fix those two bytes and copy the
 * rest verbatim. That handles IPv4 and IPv6 with one code path, which is what
 * the reference ports found necessary once online play involved AAAA records.
 */

/* The subnet-directed broadcast for this network, e.g. 192.168.1.255.
 *
 * RVGL discovers LAN games by sending to 255.255.255.255:2310 -- the hardware
 * log shows exactly that, timing out every time. The global broadcast address
 * is not routed by every stack, and libnx's is one that drops it: nothing
 * leaves the console and the connect simply expires.
 *
 * The subnet form is delivered. nifm reports the address and mask, so it can
 * be derived and substituted on the way past. Zero when unknown, in which case
 * nothing is rewritten. */
static uint32_t g_subnetBroadcast;

#define RVN_SOCKADDR_MAX 128

typedef struct { uint8_t bytes[RVN_SOCKADDR_MAX]; } HostAddress;

static int ToHostAddr(const void *guest, unsigned length, HostAddress *out) {
    if (guest == NULL || length < 2 || length > RVN_SOCKADDR_MAX) return -1;

    memset(out->bytes, 0, sizeof out->bytes);
    memcpy(out->bytes, guest, length);

    uint16_t guestFamily;
    memcpy(&guestFamily, guest, sizeof guestFamily);

    const int hostFamily = MapFamily(guestFamily);
    if (hostFamily < 0) return -1;

    if (offsetof(struct sockaddr_in, sin_family) == 1) {
        out->bytes[0] = (uint8_t)length;      /* BSD sa_len */
        out->bytes[1] = (uint8_t)hostFamily;
    } else {
        const uint16_t wide = (uint16_t)hostFamily;
        memcpy(out->bytes, &wide, sizeof wide);
    }

    /* 255.255.255.255 -> the subnet-directed form, which actually leaves the
     * console. sockaddr_in puts the address at offset 4, after family and
     * port, on both sides. */
    if (hostFamily == AF_INET && g_subnetBroadcast != 0 && length >= 8) {
        uint32_t target;
        memcpy(&target, out->bytes + 4, sizeof target);
        if (target == 0xFFFFFFFFu) {
            memcpy(out->bytes + 4, &g_subnetBroadcast, sizeof g_subnetBroadcast);

            static int announced;
            if (!announced) {
                announced = 1;
                rvnx_log_print(4, "net",
                               "rewriting 255.255.255.255 to the subnet broadcast "
                               "-- LAN discovery would otherwise go nowhere");
            }
        }
    }

    return 0;
}

static void ToGuestAddr(const HostAddress *host, unsigned hostLength,
                        void *guest, unsigned *guestLength) {
    if (guest == NULL) return;

    const unsigned capacity = (guestLength != NULL) ? *guestLength : hostLength;
    const unsigned copy = capacity < hostLength ? capacity : hostLength;
    if (copy >= 2) {
        memcpy(guest, host->bytes, copy);

        int hostFamily;
        if (offsetof(struct sockaddr_in, sin_family) == 1) {
            hostFamily = host->bytes[1];
        } else {
            uint16_t wide;
            memcpy(&wide, host->bytes, sizeof wide);
            hostFamily = wide;
        }

        const uint16_t guestFamily = (uint16_t)MapFamilyToGuest(hostFamily);
        memcpy(guest, &guestFamily, sizeof guestFamily);
    }

    if (guestLength != NULL) *guestLength = hostLength;
}

/* ------------------------------------------------------------------ */
/* Service bring-up                                                    */
/* ------------------------------------------------------------------ */

/* Counters. A netgame that does not work is otherwise completely silent --
 * these say whether anything left the console, whether anything came back,
 * and whether the socket layer was refusing calls. */
static uint64_t g_datagramsIn, g_datagramsOut, g_bytesIn, g_bytesOut;
static unsigned g_sendErrors, g_recvErrors;

static int s_socketsUp;
static int s_nifmUp;
static int s_sessions;

/* ENet opens one UDP socket per host, but RVGL also talks to the lobby/master
 * server and can run a listen socket, and libnx charges a BSD session per
 * socket. The default config is sized for a couple; this leaves room for a
 * full session plus DNS without ever hitting the ceiling mid-race. */
#ifndef RVNX_BSD_SESSIONS
#define RVNX_BSD_SESSIONS 8
#endif

/* ENet's default is 256 KB each way; matching it here avoids the driver
 * silently clamping what the engine asks for. */
#ifndef RVNX_SOCKET_BUFFER
#define RVNX_SOCKET_BUFFER (256 * 1024)
#endif

/* Network order in, dotted quad out. Three static buffers so a single log
 * line can format three addresses. */
static const char *FormatAddress(uint32_t networkOrder) {
    static char slots[3][20];
    static int next;

    char *out = slots[next];
    next = (next + 1) % 3;

    const uint8_t *octet = (const uint8_t *)&networkOrder;
    snprintf(out, sizeof slots[0], "%u.%u.%u.%u",
             octet[0], octet[1], octet[2], octet[3]);
    return out;
}

void rv_net_init(void) {
#ifdef __SWITCH__
    if (s_socketsUp) return;

    SocketInitConfig config = *socketGetDefaultInitConfig();
    config.num_bsd_sessions = RVNX_BSD_SESSIONS;
    config.tcp_tx_buf_size = RVNX_SOCKET_BUFFER;
    config.tcp_rx_buf_size = RVNX_SOCKET_BUFFER;
    config.udp_tx_buf_size = RVNX_SOCKET_BUFFER;
    config.udp_rx_buf_size = RVNX_SOCKET_BUFFER;

    Result rc = socketInitialize(&config);
    if (R_FAILED(rc)) {
        /* A tuned config can be refused when memory is tight. The default one
         * still gives working multiplayer, just with less headroom, so fall
         * back rather than losing networking entirely. */
        rc = socketInitializeDefault();
        s_sessions = 0;
    } else {
        s_sessions = RVNX_BSD_SESSIONS;
    }
    s_socketsUp = R_SUCCEEDED(rc);

    const Result nr = nifmInitialize(NifmServiceType_User);
    s_nifmUp = R_SUCCEEDED(nr);

    if (s_nifmUp) {
        u32 address = 0, subnet = 0, gateway = 0, dns1 = 0, dns2 = 0;
        if (R_SUCCEEDED(nifmGetCurrentIpConfigInfo(&address, &subnet, &gateway,
                                                   &dns1, &dns2)) &&
            address != 0 && subnet != 0) {
            /* nifm returns these ALREADY in network order -- the first octet
             * is the low byte. Reading them as host order printed
             * 197.1.168.192 for a console at 192.168.1.197, and produced a
             * subnet broadcast built from a byte-swapped address and mask,
             * which is why LAN discovery still went nowhere after the
             * rewrite was added. The mask works unchanged in either order,
             * so no conversion is wanted at all. */
            g_subnetBroadcast = (address & subnet) | ~subnet;

            rvnx_log_print(4, "net", "broadcast %s (from %s/%s)",
                           FormatAddress(g_subnetBroadcast),
                           FormatAddress(address), FormatAddress(subnet));
        }
    }

    rvnx_log_print(4, "net", "bsd %s (0x%x, %d sessions), nifm %s (0x%x)",
                   s_socketsUp ? "up" : "FAILED", rc, s_sessions,
                   s_nifmUp ? "up" : "unavailable", nr);
#else
    s_socketsUp = 1;
#endif
}

void rv_net_exit(void) {
#ifdef __SWITCH__
    if (s_nifmUp) { nifmExit(); s_nifmUp = 0; }
    if (s_socketsUp) { socketExit(); s_socketsUp = 0; }
#endif
}

int rv_net_online(void) {
#ifdef __SWITCH__
    if (!s_socketsUp) return 0;
    if (!s_nifmUp) return 1;   /* cannot ask; let the connection attempt decide */

    /* Cached for a second: RVGL's lobby UI would otherwise poll this every
     * frame and each call is an IPC round trip. */
    static u64 lastTick;
    static int lastState = -1;

    const u64 now = armGetSystemTick();
    if (lastState >= 0 && armTicksToNs(now - lastTick) < 1000000000ULL)
        return lastState;

    NifmInternetConnectionType type;
    NifmInternetConnectionStatus status;
    u32 strength = 0;
    const Result rc = nifmGetInternetConnectionStatus(&type, &strength, &status);

    lastState = R_SUCCEEDED(rc) && status == NifmInternetConnectionStatus_Connected;
    lastTick = now;
    return lastState;
#else
    return 1;
#endif
}

/* The console's own address on the LAN.
 *
 * Hosting is unusable without it: the other players have to type it in, and
 * nothing in RVGL's UI can show it here because the engine has no way to ask.
 * nifm does, so it goes in the log where it can be read off. */
void rv_net_local_address(char *buffer, size_t capacity) {
    if (buffer == NULL || capacity == 0) return;
    snprintf(buffer, capacity, "unknown");

#ifdef __SWITCH__
    if (!s_nifmUp) return;

    u32 address = 0, subnet = 0, gateway = 0, dns1 = 0, dns2 = 0;
    if (R_FAILED(nifmGetCurrentIpConfigInfo(&address, &subnet, &gateway,
                                            &dns1, &dns2)))
        return;
    if (address == 0) return;

    snprintf(buffer, capacity, "%s", FormatAddress(address));
#endif
}

void rv_net_summarise(char *buffer, size_t capacity) {
    if (buffer == NULL || capacity == 0) return;

    char address[32];
    rv_net_local_address(address, sizeof address);

    snprintf(buffer, capacity,
             "net: bsd=%s sessions=%d nifm=%s link=%s ip=%s",
             s_socketsUp ? "up" : "down",
             s_sessions ? s_sessions : -1,
             s_nifmUp ? "up" : "off",
             rv_net_online() ? "connected" : "offline",
             address);
}

void rv_net_traffic(char *buffer, size_t capacity) {
    if (buffer == NULL || capacity == 0) return;
    snprintf(buffer, capacity,
             "net traffic: sent %llu pkt / %llu KiB, received %llu pkt / %llu KiB, "
             "errors %u send %u recv",
             (unsigned long long)g_datagramsOut, (unsigned long long)(g_bytesOut >> 10),
             (unsigned long long)g_datagramsIn, (unsigned long long)(g_bytesIn >> 10),
             g_sendErrors, g_recvErrors);
}

/* ------------------------------------------------------------------ */
/* Entry points                                                        */
/* ------------------------------------------------------------------ */

int rvn_socket(int domain, int type, int protocol) {
    const int hostDomain = MapFamily(domain);
    if (hostDomain < 0) {
        Trace("socket: unsupported family %d", domain);
        errno = LX_EAFNOSUPPORT;
        return -1;
    }

    /* bionic lets SOCK_NONBLOCK ride along in `type`. */
    const int wantNonBlock = (type & B_SOCK_NONBLOCK_MASK) != 0;
    const int hostType = type & ~B_SOCK_NONBLOCK_MASK;

    const int fd = socket(hostDomain, hostType, protocol);
    if (fd < 0) return Fail(-1);

    Track(fd);

    /* ENet sets this itself, but only on the sockets it knows will broadcast.
     * Enabling it for every datagram socket costs nothing and removes one way
     * for discovery to fail silently. */
    if (hostType == SOCK_DGRAM) {
        const int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof on);
    }

    if (wantNonBlock) {
        const int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        if (fd < RVN_MAXFD) s_wantNonBlock[fd] = 1;
    }
    return fd;
}

int rvn_bind(int fd, const void *addr, unsigned addrlen) {
    HostAddress host;
    if (ToHostAddr(addr, addrlen, &host) != 0) { errno = LX_EINVAL; return -1; }
    if (bind(fd, (struct sockaddr *)host.bytes, (socklen_t)addrlen) < 0) {
        Trace("bind(fd=%d) failed: %d", fd, errno);
        return Fail(-1);
    }
    return 0;
}

int rvn_connect(int fd, const void *addr, unsigned addrlen) {
    HostAddress host;
    if (ToHostAddr(addr, addrlen, &host) != 0) { errno = LX_EINVAL; return -1; }
    if (connect(fd, (struct sockaddr *)host.bytes, (socklen_t)addrlen) < 0) {
        Trace("connect(fd=%d) failed: %d", fd, errno);
        return Fail(-1);
    }
    return 0;
}

int rvn_listen(int fd, int backlog) {
    return listen(fd, backlog) < 0 ? Fail(-1) : 0;
}

int rvn_accept(int fd, void *addr, void *addrlen) {
    HostAddress host;
    socklen_t hostLen = sizeof host.bytes;
    memset(&host, 0, sizeof host);

    const int out = accept(fd, (struct sockaddr *)host.bytes, &hostLen);
    if (out < 0) return Fail(-1);

    Track(out);
    if (addr != NULL) ToGuestAddr(&host, (unsigned)hostLen, addr, (unsigned *)addrlen);
    return out;
}

int rvn_shutdown(int fd, int how) {
    return shutdown(fd, how) < 0 ? Fail(-1) : 0;
}

int rvn_close(int fd) {
    rvn_untrack(fd);
    return close(fd) < 0 ? Fail(-1) : 0;
}

int rvn_setsockopt(int fd, int level, int name, const void *value, unsigned len) {
    const int hostLevel = MapLevel(level);
    if (hostLevel < 0) { errno = LX_ENOPROTOOPT; return -1; }

    int hostName = name;
    if (level == B_SOL_SOCKET) {
        hostName = MapSocketOption(name);
        if (hostName < 0) {
            /* Report success for options we cannot carry. ENet sets several
             * as best-effort tuning and treats a failure as fatal for the
             * whole socket, which would cost multiplayer over something like
             * SO_DEBUG. */
            return 0;
        }
    }

    /* SO_RCVTIMEO / SO_SNDTIMEO take a struct timeval whose layout matches on
     * both sides for arm64, so no conversion is needed; everything else ENet
     * sets is a plain int. */
    return setsockopt(fd, hostLevel, hostName, value, (socklen_t)len) < 0 ? Fail(-1) : 0;
}

int rvn_getsockopt(int fd, int level, int name, void *value, void *len) {
    const int hostLevel = MapLevel(level);
    if (hostLevel < 0) { errno = LX_ENOPROTOOPT; return -1; }

    int hostName = name;
    if (level == B_SOL_SOCKET) {
        hostName = MapSocketOption(name);
        if (hostName < 0) { errno = LX_ENOPROTOOPT; return -1; }
    }

    socklen_t hostLen = (len != NULL) ? (socklen_t)*(unsigned *)len : 0;
    if (getsockopt(fd, hostLevel, hostName, value, &hostLen) < 0) return Fail(-1);

    /* SO_ERROR hands the engine a raw errno, so it needs translating too. */
    if (level == B_SOL_SOCKET && name == B_SO_ERROR && value != NULL &&
        hostLen >= sizeof(int)) {
        const int saved = errno;
        errno = *(int *)value;
        *(int *)value = (*(int *)value == 0) ? 0 : TranslateErrno();
        errno = saved;
    }

    if (len != NULL) *(unsigned *)len = (unsigned)hostLen;
    return 0;
}

int rvn_getsockname(int fd, void *addr, void *addrlen) {
    HostAddress host;
    socklen_t hostLen = sizeof host.bytes;
    memset(&host, 0, sizeof host);

    if (getsockname(fd, (struct sockaddr *)host.bytes, &hostLen) < 0) return Fail(-1);
    ToGuestAddr(&host, (unsigned)hostLen, addr, (unsigned *)addrlen);
    return 0;
}

int rvn_getpeername(int fd, void *addr, void *addrlen) {
    HostAddress host;
    socklen_t hostLen = sizeof host.bytes;
    memset(&host, 0, sizeof host);

    if (getpeername(fd, (struct sockaddr *)host.bytes, &hostLen) < 0) return Fail(-1);
    ToGuestAddr(&host, (unsigned)hostLen, addr, (unsigned *)addrlen);
    return 0;
}

/* ---- the data path ---------------------------------------------------- */

#define RVN_MAX_IOV 16

long rvn_sendmsg(int fd, const void *msg, int flags) {
    if (msg == NULL) { errno = LX_EINVAL; return -1; }

    const struct BMsghdr *in = (const struct BMsghdr *)msg;
    if (in->iovlen > RVN_MAX_IOV) { errno = LX_EINVAL; return -1; }

    struct iovec iov[RVN_MAX_IOV];
    for (size_t i = 0; i < in->iovlen; ++i) {
        iov[i].iov_base = in->iov[i].base;
        iov[i].iov_len = in->iov[i].len;
    }

    HostAddress host;
    struct msghdr out;
    memset(&out, 0, sizeof out);
    out.msg_iov = iov;
    out.msg_iovlen = in->iovlen;

    if (in->name != NULL && in->namelen >= 2) {
        if (ToHostAddr(in->name, in->namelen, &host) != 0) { errno = LX_EINVAL; return -1; }
        out.msg_name = host.bytes;
        out.msg_namelen = in->namelen;
    }

    const ssize_t sent = sendmsg(fd, &out, MapSendFlags(flags));
    if (sent < 0) { ++g_sendErrors; return Fail(-1); }

    ++g_datagramsOut;
    g_bytesOut += (uint64_t)sent;
    return (long)sent;
}

long rvn_recvmsg(int fd, void *msg, int flags) {
    if (msg == NULL) { errno = LX_EINVAL; return -1; }

    struct BMsghdr *in = (struct BMsghdr *)msg;
    if (in->iovlen > RVN_MAX_IOV) { errno = LX_EINVAL; return -1; }

    struct iovec iov[RVN_MAX_IOV];
    for (size_t i = 0; i < in->iovlen; ++i) {
        iov[i].iov_base = in->iov[i].base;
        iov[i].iov_len = in->iov[i].len;
    }

    HostAddress host;
    memset(&host, 0, sizeof host);

    struct msghdr out;
    memset(&out, 0, sizeof out);
    out.msg_iov = iov;
    out.msg_iovlen = in->iovlen;
    if (in->name != NULL) {
        out.msg_name = host.bytes;
        out.msg_namelen = sizeof host.bytes;
    }

    const ssize_t got = recvmsg(fd, &out, MapSendFlags(flags));
    if (got < 0) {
        /* EAGAIN on a non-blocking socket is the normal "nothing waiting"
         * answer and is not counted; anything else is worth knowing about. */
        if (errno != EAGAIN) ++g_recvErrors;
        return Fail(-1);
    }

    /* ENet reads the peer address out of msg_name on every datagram; without
     * this write-back every packet appears to come from 0.0.0.0 and no peer
     * ever matches.
     *
     * When the kernel reports no address -- which it does for a connected
     * socket -- the old contents must be cleared rather than left. Leaving
     * them makes the datagram look like it came from whoever sent the
     * PREVIOUS one, which is worse than an obviously wrong address because
     * ENet will happily match it to a real peer. */
    if (in->name != NULL) {
        if (out.msg_namelen >= 2) {
            ToGuestAddr(&host, out.msg_namelen, in->name, &in->namelen);
        } else {
            memset(in->name, 0, in->namelen);
            in->namelen = 0;
        }
    }

    ++g_datagramsIn;
    g_bytesIn += (uint64_t)got;

    in->flags = 0;
#ifdef MSG_TRUNC
    if (out.msg_flags & MSG_TRUNC) in->flags |= B_MSG_TRUNC;
#endif

    return (long)got;
}

/* ---- name resolution --------------------------------------------------- */

int rvn_getaddrinfo(const char *node, const char *service, const void *hints,
                    void **res) {
    if (res == NULL) return -1;
    *res = NULL;

    struct addrinfo hostHints;
    memset(&hostHints, 0, sizeof hostHints);
    hostHints.ai_family = AF_INET;   /* IPv4 only, matching rvn_socket */

    if (hints != NULL) {
        const struct BAddrInfo *in = (const struct BAddrInfo *)hints;
        hostHints.ai_socktype = in->socktype;
        hostHints.ai_protocol = in->protocol;
        hostHints.ai_flags = in->flags & (AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST);
    }

    struct addrinfo *hostResult = NULL;
    const int rc = getaddrinfo(node, service, &hostHints, &hostResult);
    if (rc != 0 || hostResult == NULL) return rc != 0 ? rc : -1;

    /* Rebuild the list in bionic's layout. One allocation per node, freed by
     * rvn_freeaddrinfo; the host list is released here. */
    struct BAddrInfo *head = NULL;
    struct BAddrInfo *tail = NULL;

    for (struct addrinfo *p = hostResult; p != NULL; p = p->ai_next) {
        if (p->ai_family != AF_INET || p->ai_addr == NULL) continue;

        struct BAddrInfo *node2 = (struct BAddrInfo *)calloc(1, sizeof *node2);
        struct BSockaddrIn *addr = (struct BSockaddrIn *)calloc(1, sizeof *addr);
        if (node2 == NULL || addr == NULL) { free(node2); free(addr); break; }

        const struct sockaddr_in *src = (const struct sockaddr_in *)p->ai_addr;
        addr->family = B_AF_INET;
        addr->port = src->sin_port;
        addr->addr = src->sin_addr.s_addr;

        node2->family = B_AF_INET;
        node2->socktype = p->ai_socktype;
        node2->protocol = p->ai_protocol;
        node2->addrlen = sizeof *addr;
        node2->addr = addr;

        if (tail == NULL) head = node2; else tail->next = node2;
        tail = node2;
    }

    freeaddrinfo(hostResult);
    if (head == NULL) return -1;

    *res = head;
    return 0;
}

void rvn_freeaddrinfo(void *res) {
    struct BAddrInfo *node = (struct BAddrInfo *)res;
    while (node != NULL) {
        struct BAddrInfo *next = node->next;
        free(node->addr);
        free(node->canonname);
        free(node);
        node = next;
    }
}

int rvn_getnameinfo(const void *addr, unsigned addrlen, char *host, unsigned hostlen,
                    char *serv, unsigned servlen, int flags) {
    HostAddress hostAddr;
    if (ToHostAddr(addr, addrlen, &hostAddr) != 0) return -1;
    const struct sockaddr_in *v4 = (const struct sockaddr_in *)hostAddr.bytes;

    /* Numeric only. A reverse lookup would block the main thread for seconds
     * on a console with no resolver cache, and RVGL only uses this to render
     * an address in the server browser. */
    (void)flags;
    if (host != NULL && hostlen > 0) {
        if (inet_ntop(AF_INET, &v4->sin_addr, host, hostlen) == NULL) return -1;
    }
    if (serv != NULL && servlen > 0)
        snprintf(serv, servlen, "%u", (unsigned)ntohs(v4->sin_port));

    return 0;
}

/* ---- readiness --------------------------------------------------------- */

/* ANTI-SPIN WAITING
 * -----------------
 * libnx's poll and select can return immediately without having waited the
 * timeout they were given. ENet's enet_host_service calls enet_socket_wait
 * with a timeout every frame and loops on the result with no sleep of its
 * own, so a poll that does not block turns RVGL's netgame loop into a busy
 * spin: one core pinned, the render thread starved, and the console hot.
 *
 * The fix, taken from a port whose game waits on poll(fds, n, -1) in a
 * dedicated reader thread: slice the requested timeout, and whenever a slice
 * comes back without having actually waited, sleep the remainder explicitly.
 * After a quarter second of nothing the slices lengthen, so a genuinely idle
 * wait costs almost no wakeups.
 */
#define RVN_ACTIVE_SLICE_MS   5
#define RVN_IDLE_SLICE_MS    20
#define RVN_GOES_IDLE_AFTER  250

static int WantedEventReady(const struct pollfd *fds, unsigned long nfds) {
    for (unsigned long i = 0; i < nfds; ++i)
        if (fds[i].revents & fds[i].events) return 1;
    return 0;
}

int rvn_select(int nfds, void *readfds, void *writefds, void *exceptfds,
               void *timeout) {
    struct timeval *deadline = (struct timeval *)timeout;

    int remainingMs = -1;   /* negative: wait indefinitely */
    if (deadline != NULL)
        remainingMs = (int)(deadline->tv_sec * 1000 + deadline->tv_usec / 1000);

    struct timeval sliceTimeout = { 0, 0 };
    if (remainingMs == 0) {
        const int rc = select(nfds, (fd_set *)readfds, (fd_set *)writefds,
                              (fd_set *)exceptfds, &sliceTimeout);
        return rc < 0 ? Fail(-1) : rc;
    }

    /* select consumes its descriptor sets, so they are restored each slice. */
    fd_set readCopy, writeCopy, exceptCopy;
    if (readfds != NULL)   memcpy(&readCopy, readfds, sizeof readCopy);
    if (writefds != NULL)  memcpy(&writeCopy, writefds, sizeof writeCopy);
    if (exceptfds != NULL) memcpy(&exceptCopy, exceptfds, sizeof exceptCopy);

    const uint64_t started = NowTick();

    for (;;) {
        if (readfds != NULL)   memcpy(readfds, &readCopy, sizeof readCopy);
        if (writefds != NULL)  memcpy(writefds, &writeCopy, sizeof writeCopy);
        if (exceptfds != NULL) memcpy(exceptfds, &exceptCopy, sizeof exceptCopy);

        const int idle = ElapsedMs(started) >= RVN_GOES_IDLE_AFTER;
        int slice = idle ? RVN_IDLE_SLICE_MS : RVN_ACTIVE_SLICE_MS;
        if (remainingMs >= 0 && remainingMs < slice) slice = remainingMs;

        sliceTimeout.tv_sec = 0;
        sliceTimeout.tv_usec = idle ? 0 : slice * 1000;

        const int rc = select(nfds, (fd_set *)readfds, (fd_set *)writefds,
                              (fd_set *)exceptfds, &sliceTimeout);
        if (rc < 0) return Fail(-1);
        if (rc != 0) return rc;

        /* Whether the slice was spent waiting or returned instantly, make sure
         * it is spent before looping, so the caller cannot spin. */
        if (idle) SleepMs(slice);

        if (remainingMs >= 0) {
            remainingMs -= slice;
            if (remainingMs <= 0) return 0;
        }
    }
}

int rvn_poll(void *fds, unsigned long nfds, int timeout) {
    if (fds == NULL || nfds == 0) {
        if (timeout > 0) SleepMs(timeout);
        return 0;
    }
    if (nfds > 64) { errno = LX_EINVAL; return -1; }

    struct BPollfd *in = (struct BPollfd *)fds;
    struct pollfd host[64];

    for (unsigned long i = 0; i < nfds; ++i) {
        host[i].fd = in[i].fd;
        host[i].events = MapPollEventsIn(in[i].events);
        host[i].revents = 0;
    }

    const uint64_t started = NowTick();
    int remainingMs = timeout;   /* negative: wait indefinitely */

    for (;;) {
        const int idle = ElapsedMs(started) >= RVN_GOES_IDLE_AFTER;
        int slice = idle ? RVN_IDLE_SLICE_MS : RVN_ACTIVE_SLICE_MS;
        if (timeout == 0) slice = 0;
        else if (remainingMs >= 0 && remainingMs < slice) slice = remainingMs;

        for (unsigned long i = 0; i < nfds; ++i) host[i].revents = 0;

        const int rc = poll(host, (nfds_t)nfds, idle ? 0 : slice);
        if (rc < 0) return Fail(-1);

        if (rc > 0 && WantedEventReady(host, nfds)) {
            for (unsigned long i = 0; i < nfds; ++i)
                in[i].revents = MapPollEventsOut(host[i].revents);
            return rc;
        }

        /* A descriptor in an error state is still a result, but a caller that
         * keeps polling instead of acting on it must not be able to spin, so
         * it pays the slice first. */
        if (rc > 0) {
            SleepMs(slice);
            for (unsigned long i = 0; i < nfds; ++i)
                in[i].revents = MapPollEventsOut(host[i].revents);
            return rc;
        }

        if (timeout == 0) {
            for (unsigned long i = 0; i < nfds; ++i) in[i].revents = 0;
            return 0;
        }

        if (idle) SleepMs(slice);

        if (remainingMs >= 0) {
            remainingMs -= slice;
            if (remainingMs <= 0) {
                for (unsigned long i = 0; i < nfds; ++i) in[i].revents = 0;
                return 0;
            }
        }
    }
}

int rvn_fcntl(int fd, int cmd, ...) {
    va_list args;
    va_start(args, cmd);
    const int arg = va_arg(args, int);
    va_end(args);

    switch (cmd) {
        case B_F_GETFL: {
            const int flags = fcntl(fd, F_GETFL, 0);
            if (flags < 0) return Fail(-1);
            /* Report the bionic bit, not the host one. ENet does a
             * read-modify-write of these flags, so handing back the host value
             * would make it clear O_NONBLOCK on the next F_SETFL. */
            return (flags & O_NONBLOCK) ? B_O_NONBLOCK : 0;
        }
        case B_F_SETFL: {
            int flags = fcntl(fd, F_GETFL, 0);
            if (flags < 0) return Fail(-1);
            if (arg & B_O_NONBLOCK) flags |= O_NONBLOCK;
            else                    flags &= ~O_NONBLOCK;
            if (fd >= 0 && fd < RVN_MAXFD)
                s_wantNonBlock[fd] = (uint8_t)((arg & B_O_NONBLOCK) != 0);
            return fcntl(fd, F_SETFL, flags) < 0 ? Fail(-1) : 0;
        }
        case B_F_GETFD:
            return 0;
        case B_F_SETFD:
            return 0;   /* FD_CLOEXEC is meaningless without exec() */
        default:
            errno = LX_EINVAL;
            return -1;
    }
}

int rvn_ioctl(int fd, unsigned long request, ...) {
    va_list args;
    va_start(args, request);
    void *arg = va_arg(args, void *);
    va_end(args);

    switch (request) {
        case B_FIONBIO: {
            const int on = (arg != NULL) ? *(int *)arg : 0;
            int flags = fcntl(fd, F_GETFL, 0);
            if (flags < 0) return Fail(-1);
            if (on) flags |= O_NONBLOCK; else flags &= ~O_NONBLOCK;
            if (fd >= 0 && fd < RVN_MAXFD) s_wantNonBlock[fd] = (uint8_t)(on != 0);
            return fcntl(fd, F_SETFL, flags) < 0 ? Fail(-1) : 0;
        }
        case B_FIONREAD: {
#ifdef FIONREAD
            return ioctl(fd, FIONREAD, arg) < 0 ? Fail(-1) : 0;
#else
            errno = LX_ENOTTY;
            return -1;
#endif
        }
        default:
            errno = LX_ENOTTY;
            return -1;
    }
}

/* ---- close interception ------------------------------------------------
 *
 * RVGL imports `close` once, and it routes through libc_shim's close_fake so
 * fake pipe fds keep working. Socket fds therefore never reach rvn_close, and
 * without this hook a reused fd number would still be flagged as a socket by
 * a later call. imports.c points `close` here. */

int close_fake(int fd);   /* libc_shim.c */

int rvn_close_hook(int fd) {
    rvn_untrack(fd);
    return close_fake(fd);
}
