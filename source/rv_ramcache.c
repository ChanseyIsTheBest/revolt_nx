/* rv_ramcache.c -- the shared RAM budget asset_pack.c draws on.
 *
 * In the port this pack came from, these live in that project's (much larger)
 * libc_shim.c, which this tree does not use. The API is tiny and the policy
 * belongs to the game, so it lives here instead.
 *
 * What it buys: asset_pack's load_pack_blob() asks for the whole .nxpack up
 * front. If the budget covers it, the entire archive is held in memory and
 * every asset read becomes a memcpy -- no SD traffic at all after boot. If it
 * does not, the request is refused and the pack is read from the card exactly
 * as before. Both outcomes are correct; only one is fast.
 *
 * Failing to fit is therefore not an error and is not treated as one. That is
 * what makes a generous default safe: the worst case is the behaviour we
 * already had.
 *
 * MIT licensed.
 */

#include <pthread.h>
#include <stdio.h>
#include <sys/stat.h>

#include "config.h"
#include "rv_log.h"
#include "rv_ramcache.h"

/* Budget in megabytes.
 *
 * Sized against what is actually in play: ~280 MB of mapped modules, RVGL's
 * own 168 MB of .bss, and a pack that is a few hundred MB for a full install.
 * Switch applications get roughly 3.2 GB in title-override mode, so 640 leaves
 * a wide margin while still covering a complete RVGL data set.
 *
 * Build with -DRVNX_RAM_CACHE_MB=0 to turn it off. */
#ifndef RVNX_RAM_CACHE_MB
#define RVNX_RAM_CACHE_MB 640
#endif

int bp_ram_cache_mb = RVNX_RAM_CACHE_MB;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static long g_budget = -1;          /* bytes left; -1 = not yet initialised */
static long g_peak;

void rv_ramcache_configure(const char *baseDir) {
    /* A zero-rebuild off switch, for the same reason the pack has one: this
     * decides whether several hundred MB get allocated at boot. */
    if (baseDir != NULL) {
        char path[512];
        snprintf(path, sizeof path, "%s/noramcache.flag", baseDir);
        struct stat info;
        if (stat(path, &info) == 0) {
            bp_ram_cache_mb = 0;
            rvnx_log_print(4, "ramcache", "noramcache.flag present -- disabled");
            return;
        }
    }

    rvnx_log_print(4, "ramcache", "budget %d MB", bp_ram_cache_mb);
}

long bp_ram_cache_take(long bytes) {
    if (bytes <= 0) return 0;

    pthread_mutex_lock(&g_lock);
    if (g_budget < 0) g_budget = (long)bp_ram_cache_mb << 20;

    /* All or nothing. A partial grant would leave the caller holding memory it
     * cannot use, and every caller here wants a whole file or none of it. */
    const long granted = (bytes <= g_budget) ? bytes : 0;
    g_budget -= granted;
    if (granted > 0) {
        const long held = ((long)bp_ram_cache_mb << 20) - g_budget;
        if (held > g_peak) g_peak = held;
    }
    pthread_mutex_unlock(&g_lock);

    return granted;
}

void bp_ram_cache_give(long bytes) {
    if (bytes <= 0) return;
    pthread_mutex_lock(&g_lock);
    g_budget += bytes;
    pthread_mutex_unlock(&g_lock);
}

void rv_ramcache_stats(char *buffer, size_t capacity) {
    if (buffer == NULL || capacity == 0) return;

    pthread_mutex_lock(&g_lock);
    const long total = (long)bp_ram_cache_mb << 20;
    const long held = g_budget < 0 ? 0 : total - g_budget;
    pthread_mutex_unlock(&g_lock);

    snprintf(buffer, capacity, "ramcache: %ld of %ld MB held, peak %ld MB",
             held >> 20, total >> 20, g_peak >> 20);
}
