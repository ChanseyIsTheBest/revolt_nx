/* imports_helpers.c -- shim bodies that live in the reference ports'
 * imports.c rather than in libc_shim.c.
 *
 * Copyright (C) 2021 Andy Nguyen, fgsfds. MIT licensed; see LICENSE.
 * Extracted from sonicjump/source/imports.c and de-Unity-ised.
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * tools/gen_imports.py reuses the resolution expression each reference port
 * already uses for a symbol. That is right for the ~120 entries backed by
 * libc_shim.c, and wrong for these: they are defined *inside* the reference
 * imports.c, several of them `static`, so copying the expression alone left
 * 32 dangling references and a tree that compiles file-by-file and then fails
 * to link. Bringing the bodies over is the fix; the audit that caught it is
 * tools/check_links.py.
 *
 * The pthread shims are the important ones and the reason this could not just
 * be pointed at newlib. bionic allocates pthread_mutex_t, pthread_cond_t and
 * pthread_once_t inline in the caller's memory and zero-initialises them;
 * newlib's are a different size and are not zero-initialisable. So the bionic
 * storage is reinterpreted as a pointer slot and lazily backed with a real
 * newlib object. pthread_create_fake matters even more: it is what gives every
 * engine-spawned thread its own bionic TLS block, without which the first
 * stack-protector prologue on that thread reads a canary out of libnx's
 * thread struct. There are 15,198 such prologues in libpapapearsaga.so.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <math.h>
#include <switch.h>

#include "config.h"
#include "rv_log.h"
#include "util.h"
#include "libc_shim.h"
#include "compat_stubs.h"
#include "imports_helpers.h"
#include "pps_perf.h"
#include "pps_diag.h"
#include "pps_jni.h"   /* jni_quit_requested */

uint64_t __stack_chk_guard_fake = 0x0ull; /* match install_bionic_tls's zeroed tpidr+0x28 slot */
void __stack_chk_fail_fake(void) {  abort(); }

int  __cxa_atexit_fake(void (*fn)(void *), void *arg, void *dso) { (void)fn; (void)arg; (void)dso; return 0; }
void __cxa_finalize_fake(void *dso) { (void)dso; }

// stdin/stdout/stderr point into the fake __sF block (see libc_shim.c)
FILE *stderr_fake = (FILE *)&fake_sF[2];

// ---------------------------------------------------------------------------
// pthread: bionic allocates the opaque types inline and zero-inits them, so we
// lazily back them with heap-allocated newlib objects stashed through the
// caller's pointer slot.
// ---------------------------------------------------------------------------

/* ---------------------------------------------------------------------------
 * ALIGNMENT, and why the slot is 32 bits wide.
 *
 * The version this replaces stored a real newlib mutex/cond POINTER in the
 * first 8 bytes of the bionic object and read it with 64-bit atomics -- an
 * ldar on the way in, a 64-bit compare-exchange to back it. That is only
 * legal if the bionic object is 8-byte aligned, and on arm64 it is not
 * guaranteed to be: bionic's pthread_mutex_t is int32_t __private[10] and its
 * pthread_cond_t is int32_t __private[12], both with 4-byte alignment. This
 * engine embeds a condvar at struct offset 0x194. The tenth hardware run was
 *
 *     ensure_cond+0x0c   ldar x19, [x0]    x0 = 0x...314
 *
 * an alignment fault: unlike a plain ldr, the acquire and exclusive forms
 * require natural alignment. Both reference ports got away with the 64-bit
 * design because their engines happened to place every lock at an 8-aligned
 * offset. That is luck, not correctness.
 *
 * So the slot is now the object's FIRST 32-BIT WORD -- which bionic itself
 * uses as the state word, and which is what the static initialisers write:
 * 0 (normal), 0x4000 (recursive), 0x8000 (errorcheck). A backed object has
 * bit 31 set and an index into a table of real newlib objects in the low
 * bits. 32-bit atomics on a 4-aligned address are always legal. Everything
 * else -- the race-free backing, the recursive detection, the wait caps --
 * is unchanged.
 * ------------------------------------------------------------------------- */
/* The cap is the QUANTUM A LOST SIGNAL COSTS, and the measurement showed that
 * is what this engine pays on every wait: 24 waits x 16 ms = 384 ms of a
 * single frame, with the predicate satisfied long before.
 *
 * 16 ms was chosen as "one frame at 60 Hz, so a lost wakeup costs a frame
 * rather than the session". That reasoning holds for a wait that happens
 * occasionally. Here the main thread waits dozens of times per stall, so the
 * quantum is multiplied by the depth of the dependency chain and one frame
 * becomes twenty-four.
 *
 * Dropping it to 1 ms was tried and is the experiment that settled what this
 * is. The stall did not shrink -- 24 waits x 16 ms became 371 waits x 1 ms,
 * 384 ms became 372 ms. The TOTAL is unchanged, so nothing is being lost to a
 * missed signal; the waiter is genuinely waiting that long and the cap only
 * sets how often it re-checks. 16 ms restored: same result, far fewer
 * wakeups. */
#define COND_WAIT_CAP_MS 16

#define SLOT_BACKED   0x80000000u
#define SLOT_INDEX(v) ((v) & 0x7fffffffu)
#define MAX_SYNC_OBJS 4096

static pthread_mutex_t *g_mtx_tab[MAX_SYNC_OBJS];
static pthread_cond_t  *g_cnd_tab[MAX_SYNC_OBJS];
static int g_mtx_n, g_cnd_n;            /* next free index, atomically bumped */

static pthread_mutex_t *make_real_mutex(int recursive) {
  pthread_mutex_t *m = calloc(1, sizeof(*m));
  int rc;
  if (!m) return NULL;
  if (recursive) {
    pthread_mutexattr_t a; pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    rc = pthread_mutex_init(m, &a); pthread_mutexattr_destroy(&a);
  } else {
    rc = pthread_mutex_init(m, NULL);
  }
  if (rc != 0) { free(m); return NULL; }
  return m;
}

/* Back the bionic object at `slot` if it is not already, and return the real
 * newlib object. Race-free: the 32-bit compare-exchange means one thread wins
 * the backing and the loser adopts the winner's object. */
static pthread_mutex_t *mutex_of(void *slot, int force_recursive) {
  uint32_t *w = slot;
  uint32_t cur, expected;
  int idx;
  pthread_mutex_t *m;
  if (!w) return NULL;
  cur = __atomic_load_n(w, __ATOMIC_ACQUIRE);
  if (cur & SLOT_BACKED) return g_mtx_tab[SLOT_INDEX(cur)];

  m = make_real_mutex(force_recursive || cur == 0x4000);
  if (!m) return NULL;
  idx = __atomic_fetch_add(&g_mtx_n, 1, __ATOMIC_ACQ_REL);
  if (idx >= MAX_SYNC_OBJS) { pthread_mutex_destroy(m); free(m); return NULL; }
  g_mtx_tab[idx] = m;

  expected = cur;
  if (!__atomic_compare_exchange_n(w, &expected, SLOT_BACKED | (uint32_t)idx,
                                   0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    /* Lost the race: another thread backed it first. Ours is now orphaned in
     * the table -- a leak of one mutex, which is fine -- and we use theirs. */
    return g_mtx_tab[SLOT_INDEX(expected)];
  }
  return m;
}

/* Every wait in the instrumented run was an exact multiple of
 * COND_WAIT_CAP_MS -- 24 waits, 384.1 ms, 16.004 ms each, never a partial
 * slice. A waiter woken by a signal produces a SHORT final slice, so the
 * absence of one is proof that the signal never reaches it and every wakeup
 * is our timeout.
 *
 * The engine's side is not at fault: its calls are thin libc++ wrappers that
 * pass the condvar and mutex pointers straight through, checked in the
 * disassembly. So signal and wait are resolving to different backing objects,
 * or the signal lands when nobody is waiting. This records which, cheaply and
 * only for the first few hundred operations. */
static void cond_diag(const char *what, const void *slot, int idx) {
#if PPS_COND_DIAG
  static int n;
  if (n < 400) { n++; LOGB("cond %-9s slot=%p idx=%d", what, slot, idx); }
#else
  (void)what; (void)slot; (void)idx;
#endif
}

static pthread_cond_t *cond_of(void *slot) {
  uint32_t *w = slot;
  uint32_t cur, expected;
  int idx;
  pthread_cond_t *c;
  if (!w) return NULL;
  cur = __atomic_load_n(w, __ATOMIC_ACQUIRE);
  if (cur & SLOT_BACKED) return g_cnd_tab[SLOT_INDEX(cur)];

  c = calloc(1, sizeof(*c));
  if (!c) return NULL;
  if (pthread_cond_init(c, NULL) != 0) { free(c); return NULL; }
  idx = __atomic_fetch_add(&g_cnd_n, 1, __ATOMIC_ACQ_REL);
  if (idx >= MAX_SYNC_OBJS) { pthread_cond_destroy(c); free(c); return NULL; }
  g_cnd_tab[idx] = c;

  expected = cur;
  if (!__atomic_compare_exchange_n(w, &expected, SLOT_BACKED | (uint32_t)idx,
                                   0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return g_cnd_tab[SLOT_INDEX(expected)];
  return c;
}

int pthread_mutex_init_fake(pthread_mutex_t **uid, const int *attr) {
  /* bionic PTHREAD_MUTEX_RECURSIVE == 1. Reset the word first so a re-init of
   * a previously backed object gets a fresh mutex rather than the old one. */
  const int recursive = (attr && *attr == 1);
  if (!uid) return -1;
  __atomic_store_n((uint32_t *)uid, recursive ? 0x4000u : 0u, __ATOMIC_RELEASE);
  return mutex_of(uid, recursive) ? 0 : -1;
}
int pthread_mutex_destroy_fake(pthread_mutex_t **uid) {
  uint32_t cur;
  if (!uid) return 0;
  cur = __atomic_exchange_n((uint32_t *)uid, 0u, __ATOMIC_ACQ_REL);
  if (cur & SLOT_BACKED) {
    pthread_mutex_t *m = g_mtx_tab[SLOT_INDEX(cur)];
    g_mtx_tab[SLOT_INDEX(cur)] = NULL;
    if (m) { pthread_mutex_destroy(m); free(m); }
  }
  return 0;
}
int pthread_mutex_lock_fake(pthread_mutex_t **uid) {
  pthread_mutex_t *m = mutex_of(uid, 0);
  if (!m) return -1;
  /* Only a CONTENDED lock is a wait worth beaconing. */
  if (pthread_mutex_trylock(m) == 0) return 0;
  diag_wait_enter(DIAG_W_MUTEX, m);
  { const uint64_t t0 = perf_begin();
    const int r = pthread_mutex_lock(m);
    perf_end(PERF_MUTEX, t0, NULL);
    diag_wait_exit(); return r; }
}
int pthread_mutex_trylock_fake(pthread_mutex_t **uid) { pthread_mutex_t *m = mutex_of(uid, 0); return m ? pthread_mutex_trylock(m) : -1; }
int pthread_mutex_unlock_fake(pthread_mutex_t **uid)  { pthread_mutex_t *m = mutex_of(uid, 0); return m ? pthread_mutex_unlock(m) : -1; }
int pthread_mutex_timedlock_fake(pthread_mutex_t **uid, const struct timespec *abs) {
  pthread_mutex_t *m = mutex_of(uid, 0);
  (void)abs;
  if (!m) return -1;
  for (int i = 0; i < 1000; i++) {
    if (pthread_mutex_trylock(m) == 0) return 0;
    svcSleepThread(1000000ull);
  }
  return ETIMEDOUT;
}

int pthread_cond_init_fake(pthread_cond_t **cnd, const int *attr) {
  (void)attr;
  if (!cnd) return -1;
  __atomic_store_n((uint32_t *)cnd, 0u, __ATOMIC_RELEASE);
  return cond_of(cnd) ? 0 : -1;
}
int pthread_cond_broadcast_fake(pthread_cond_t **cnd) {
  pthread_cond_t *c = cond_of(cnd);
  cond_diag("broadcast", cnd, c ? (int)(SLOT_INDEX(__atomic_load_n((uint32_t *)cnd, __ATOMIC_ACQUIRE))) : -1);
  return c ? pthread_cond_broadcast(c) : -1;
}
int pthread_cond_signal_fake(pthread_cond_t **cnd) {
  pthread_cond_t *c = cond_of(cnd);
  cond_diag("signal", cnd, c ? (int)(SLOT_INDEX(__atomic_load_n((uint32_t *)cnd, __ATOMIC_ACQUIRE))) : -1);
  return c ? pthread_cond_signal(c) : -1;
}
int pthread_cond_destroy_fake(pthread_cond_t **cnd) {
  uint32_t cur;
  if (!cnd) return 0;
  cur = __atomic_exchange_n((uint32_t *)cnd, 0u, __ATOMIC_ACQ_REL);
  if (cur & SLOT_BACKED) {
    pthread_cond_t *c = g_cnd_tab[SLOT_INDEX(cur)];
    g_cnd_tab[SLOT_INDEX(cur)] = NULL;
    if (c) { pthread_cond_destroy(c); free(c); }
  }
  return 0;
}

/* Cap the UNTIMED wait too. A raced or lost signal -- or a static-cond object
 * mismatch across signal/wait -- would otherwise park the engine forever.
 * Waking every COND_WAIT_CAP_MS and returning as a spurious wakeup lets the
 * caller re-check its predicate, which POSIX permits and every correct waiter
 * handles. Inherited from the reference ports; see their notes. */
int pthread_cond_wait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx) {
  pthread_cond_t *c = cond_of(cnd);
  pthread_mutex_t *m = mutex_of(mtx, 0);
  struct timespec cap;
  uint64_t perf_t0;
  long add;
  int r;
  if (!c || !m) return -1;
  cond_diag("wait", cnd, (int)(SLOT_INDEX(__atomic_load_n((uint32_t *)cnd, __ATOMIC_ACQUIRE))));
  diag_wait_enter(DIAG_W_COND, cnd);
  perf_t0 = perf_begin();
  clock_gettime(CLOCK_MONOTONIC, &cap);
  add = COND_WAIT_CAP_MS * 1000000L;
  cap.tv_sec  += (cap.tv_nsec + add) / 1000000000L;
  cap.tv_nsec  = (cap.tv_nsec + add) % 1000000000L;
  r = pthread_cond_timedwait(c, m, &cap);
  perf_end(PERF_COND, perf_t0, NULL);
  diag_wait_exit();
  return (r == ETIMEDOUT) ? 0 : r;
}
/* THE CALLER'S DEADLINE IS CLOCK_REALTIME. Read out of the engine, not
 * assumed: its one call site is libc++'s __libcpp_condvar_timedwait, and the
 * disassembly shows the deadline built from
 * `system_clock::time_point::time_since_epoch()` -- nanoseconds since the Unix
 * epoch, split into a timespec by a divide-by-1e9.
 *
 * This used to compare that against a CLOCK_MONOTONIC cap:
 *
 *     if (t->tv_sec < cap.tv_sec) use = t;      // epoch vs uptime
 *
 * An epoch second is ~1.79e9 and an uptime second is a few hundred, so the
 * test was never true and the caller's deadline was never used. Every timed
 * wait became a flat COND_WAIT_CAP_MS. All three reference ports have the same
 * line, so no port in this family has ever honoured an engine's timeout.
 *
 * It is converted properly now: the deadline becomes a remaining DURATION
 * against the same clock it was expressed in, and the wait is the shorter of
 * that and the cap. A caller asking for 2 ms gets 2 ms instead of 16.
 *
 * Note this does not shorten a wait the engine means to take: libc++ ignores
 * the return value here and loops until system_clock::now() passes its own
 * deadline. It stops us inventing timeouts the caller did not ask for. */
int pthread_cond_timedwait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx, const struct timespec *t) {
  pthread_cond_t *c = cond_of(cnd);
  pthread_mutex_t *m = mutex_of(mtx, 0);
  struct timespec now, use;
  int64_t remain_ns = (int64_t)COND_WAIT_CAP_MS * 1000000;
  const int64_t cap_ns = remain_ns;

  if (!c || !m) return -1;

  if (t) {
    /* Same clock the caller used, so the subtraction is meaningful. */
    if (clock_gettime(CLOCK_REALTIME, &now) == 0) {
      const int64_t d = ((int64_t)t->tv_sec - (int64_t)now.tv_sec) * 1000000000LL
                      + ((int64_t)t->tv_nsec - (int64_t)now.tv_nsec);
      if (d < remain_ns) remain_ns = d;
    }
  }
  /* NEVER ZERO. A zero-length timed wait returns immediately, and libc++'s
   * wait_until loops on its own deadline -- so a deadline the clock thinks has
   * already passed would turn into a busy loop on the game thread, at 100% of
   * a core, which is precisely the symptom this port has been trying to remove.
   *
   * That is a real risk rather than a hypothetical: this now subtracts two
   * CLOCK_REALTIME readings, and if that clock is coarse on this platform the
   * difference can come out negative for a deadline only microseconds away.
   * A floor of one millisecond costs nothing and makes the failure mode
   * "slightly late" instead of "spins a core". */
  if (remain_ns < 1000000) remain_ns = 1000000;
  if (remain_ns > cap_ns)  remain_ns = cap_ns;

  /* The wait itself is expressed against whatever clock newlib's
   * pthread_cond_timedwait uses, so build it from that clock's own now(). */
  if (clock_gettime(CLOCK_REALTIME, &now) != 0) return -1;
  use.tv_sec  = now.tv_sec + (now.tv_nsec + remain_ns) / 1000000000LL;
  use.tv_nsec = (now.tv_nsec + remain_ns) % 1000000000LL;

  { const uint64_t t0 = perf_begin();
    const int r = pthread_cond_timedwait(c, m, &use);
    perf_end(PERF_COND, t0, NULL);
    return r; }
}

int pthread_once_fake(volatile int *once, void (*init)(void)) {
  if (!once || !init) return -1;
  if (__sync_lock_test_and_set(once, 1) == 0) (*init)();
  return 0;
}

int pthread_mutexattr_init_fake(int *a) { if (a) *a = 0; return 0; }
int pthread_mutexattr_settype_fake(int *a, int t) { if (a) *a = t; return 0; }

// bionic pthread_attr_t is opaque storage we own; stash size/detach there
#define ATTR_MAGIC 0x41545452 /* 'ATTR' */
typedef struct { uint32_t magic; uint32_t detach; size_t stacksize; } OurAttr;

int pthread_attr_init_fake(void *a) { if (a) { OurAttr *o = a; o->magic = ATTR_MAGIC; o->detach = 0; o->stacksize = 0; } return 0; }
int pthread_attr_destroy_fake(void *a) { (void)a; return 0; }
int pthread_attr_setdetachstate_fake(void *a, int s) { if (a) { OurAttr *o = a; if (o->magic == ATTR_MAGIC) o->detach = (uint32_t)s; } return 0; }
int pthread_attr_setstacksize_fake(void *a, size_t s) { if (a) { OurAttr *o = a; if (o->magic == ATTR_MAGIC) o->stacksize = s; } return 0; }
int pthread_attr_getstacksize_fake(const void *a, size_t *s) { if (s) { const OurAttr *o = a; *s = (a && o->magic == ATTR_MAGIC && o->stacksize) ? o->stacksize : (512 * 1024); } return 0; }
int pthread_attr_setschedparam_fake(void *a, const void *p) { (void)a; (void)p; return 0; }

typedef struct { void *(*entry)(void *); void *arg; uint8_t tls[BIONIC_TLS_SIZE]; } ThreadStart;
static void *thread_trampoline(void *p) {
  ThreadStart *ts = (ThreadStart *)p;   /* leaked on purpose: tpidr points into ts->tls */
  install_bionic_tls(ts->tls);
  /* Register here rather than in pthread_create_fake: the registry records
   * the thread's own kernel handle and id, which only exist once it is
   * actually running. This is also the exact point where it becomes capable
   * of running engine code, which is what the watchdog cares about. */
  diag_thread_register(ts->entry, 0);
  void *r = ts->entry(ts->arg);
  diag_thread_unregister();
  return r;
}
int pthread_create_fake(pthread_t *thread, const void *bionic_attr, void *entry, void *arg) {
  /* nativeActivateGame spawns loadWhileShowingSplash here. A thread that never
   * starts and a thread that hangs look identical from the outside, so record
   * the entry point. */
  LOGB("thread: pthread_create entry=%p", entry);

  ThreadStart *ts = malloc(sizeof(*ts));
  if (!ts) return -1;
  ts->entry = (void *(*)(void *))entry;
  ts->arg = arg;
  size_t stack = 0;
  if (bionic_attr) {
    const OurAttr *o = bionic_attr;
    if (o->magic == ATTR_MAGIC) stack = o->stacksize;
  }
  if (stack < (2u << 20)) stack = 2u << 20; // 2 MB floor for the heavy engine threads
  pthread_attr_t attr; pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, stack);
  const int r = pthread_create(thread, &attr, thread_trampoline, ts);
  pthread_attr_destroy(&attr);
  if (r != 0) { free(ts); return r; }
  return 0;
}
int pthread_join_fake(pthread_t thread, void **retval) {
  diag_wait_enter(DIAG_W_JOIN, (const void *)(uintptr_t)thread);
  const int r = pthread_join(thread, retval);
  diag_wait_exit();
  return r;
}
int pthread_setschedparam_fake(pthread_t t, int policy, const void *p) { (void)t; (void)policy; (void)p; return 0; }
int pthread_sigmask_fake(int how, const void *set, void *old) { (void)how; (void)set; (void)old; return 0; }
int pthread_kill_fake(pthread_t t, int sig) { (void)t; (void)sig; return 0; }


// ---------------------------------------------------------------------------
// pthread TLS keys, multiplexed over a single real newlib key.
// devkitA64 backs pthread keys with a tiny pool (~16 libnx TLS slots), but
// Unity's runtime creates dozens during init (46 call sites). The ~17th
// pthread_key_create returns EAGAIN, and libunity treats that as fatal
// (asserts the key was created, else BRK). bionic allows 128 keys; emulate
// that: one real key holds a per-thread value array for up to 128 fake keys.
// ---------------------------------------------------------------------------
#define FAKE_KEYS_MAX 128
static pthread_mutex_t g_key_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct { int used; void (*dtor)(void *); } g_key_table[FAKE_KEYS_MAX];
static pthread_key_t g_master_key;
static int g_master_key_ready;
typedef struct { void *values[FAKE_KEYS_MAX]; } KeyValues;

static void master_key_dtor(void *p) {
  KeyValues *kv = p;
  for (int iter = 0; iter < 4; iter++) {     // POSIX: rerun while dtors set new values
    int again = 0;
    for (int i = 0; i < FAKE_KEYS_MAX; i++) {
      void *v = kv->values[i];
      if (g_key_table[i].used && g_key_table[i].dtor && v) {
        kv->values[i] = NULL;
        g_key_table[i].dtor(v);
        again = 1;
      }
    }
    if (!again) break;
  }
  free(kv);
}

int pthread_key_create_fake(unsigned *key, void (*dtor)(void *)) {
  pthread_mutex_lock(&g_key_mutex);
  if (!g_master_key_ready) {
    if (pthread_key_create(&g_master_key, master_key_dtor) != 0) {
      pthread_mutex_unlock(&g_key_mutex);
      
      return EAGAIN;
    }
    g_master_key_ready = 1;
  }
  for (unsigned i = 0; i < FAKE_KEYS_MAX; i++) {
    if (!g_key_table[i].used) {
      g_key_table[i].used = 1;
      g_key_table[i].dtor = dtor;
      *key = i + 1;                 // 1-based: a zeroed key is invalid
      pthread_mutex_unlock(&g_key_mutex);
      return 0;
    }
  }
  pthread_mutex_unlock(&g_key_mutex);
  
  return EAGAIN;
}

int pthread_key_delete_fake(unsigned key) {
  if (key == 0 || key > FAKE_KEYS_MAX) return EINVAL;
  pthread_mutex_lock(&g_key_mutex);
  g_key_table[key - 1].used = 0;
  g_key_table[key - 1].dtor = NULL;
  pthread_mutex_unlock(&g_key_mutex);
  return 0;
}

void *pthread_getspecific_fake(unsigned key) {
  if (key == 0 || key > FAKE_KEYS_MAX || !g_master_key_ready) return NULL;
  KeyValues *kv = pthread_getspecific(g_master_key);
  return kv ? kv->values[key - 1] : NULL;
}

int pthread_setspecific_fake(unsigned key, const void *value) {
  if (key == 0 || key > FAKE_KEYS_MAX || !g_master_key_ready) return EINVAL;
  KeyValues *kv = pthread_getspecific(g_master_key);
  if (!kv) {
    kv = calloc(1, sizeof(*kv));
    if (!kv) return ENOMEM;
    pthread_setspecific(g_master_key, kv);
  }
  kv->values[key - 1] = (void *)value;
  return 0;
}


int z_strcmp(const char *a, const char *b) {
  if (a == b) return 0;
  if (!a) return -1;
  if (!b) return 1;
  return strcmp(a, b);
}
int z_strncmp(const char *a, const char *b, size_t n) {
  if (a == b || n == 0) return 0;
  if (!a) return -1;
  if (!b) return 1;
  return strncmp(a, b, n);
}
char *z_strstr(const char *h, const char *n) {
  if (!h || !n) return NULL;
  return strstr(h, n);
}
char *z_strchr(const char *s, int c) { return s ? strchr(s, c) : NULL; }
char *z_strrchr(const char *s, int c) { return s ? strrchr(s, c) : NULL; }
size_t z_strlen(const char *s) { return s ? strlen(s) : 0; }


/* --- small stubs that were static in the reference imports.c --------------- */

int  ret0_i(void) { return 0; }
int  signal_stub(int sig, void *handler) { (void)sig; (void)handler; return 0; }
long sysconf_pass(int name) { return sysconf_fake(name); }

/* sincos is a GNU extension and newlib has neither spelling.
 *
 * sincosf_fake is NOT defined here: libc_shim.c already has one, and defining
 * it in both is a duplicate-symbol link error. Only the double-precision form
 * is missing upstream. Caught by the whole-tree duplicate scan, not by any
 * per-file compile -- both files were individually clean. */
void sincos_fake(double x, double *s, double *c) { *s = sin(x); *c = cos(x); }

/* See the note in imports_helpers.h. The engine calls exit() from its console
 * "quit" command and from LaunchResetProgress; both want the normal shutdown
 * path, not process death. */
void pps_exit(int code) {
  (void)code;
  jni_quit_requested = 1;
  for (;;) svcSleepThread(16000000ull);
}
