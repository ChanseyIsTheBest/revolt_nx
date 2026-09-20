/* rv_ramcache.h -- shared RAM budget for the asset pack. See rv_ramcache.c.
 * MIT licensed. */
#ifndef RVNX_RV_RAMCACHE_H
#define RVNX_RV_RAMCACHE_H

#include <stddef.h>

/* Megabytes the cache may hold. asset_pack.c reads this directly. */
extern int bp_ram_cache_mb;

/* Reads noramcache.flag and logs the budget. Call once, after paths resolve
 * and before the pack is opened. */
void rv_ramcache_configure(const char *baseDir);

/* All or nothing: returns `bytes` if the budget covers it, else 0. A refusal
 * is a normal outcome, not an error -- the caller reads from the card. */
long bp_ram_cache_take(long bytes);
void bp_ram_cache_give(long bytes);

void rv_ramcache_stats(char *buffer, size_t capacity);

#endif
