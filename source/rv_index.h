/* rv_index.h -- in-memory index of the game data tree.
 *
 * WHY
 * ---
 * From a hardware boot log, RVGL issues several thousand fopen calls before it
 * reaches the menu, and a large share of them are expected misses: it probes
 * levels/<name>/reversed/<name>.inf for all 21 levels, cache/shaders/ entries for
 * every shader variant, alternate texture extensions for every page. On FAT32
 * over the Switch's SD interface each of those failures walks a directory.
 *
 * So: walk the data tree once at boot and keep a hash index of every file.
 * After that a miss is a memory read rather than a directory walk.
 *
 * The idea and the shape of this are taken from the bloonspop port's
 * bp_assets.c, which solved the same problem for Unity's Resources and
 * Addressables layers. Two things from its design are worth restating because
 * they are what make it safe:
 *
 *   It does NOT cache contents. This is an index of names. A hit still opens
 *   the file normally; only the miss is short-circuited.
 *
 *   It is NOT authoritative. A path outside the tree, or a write, or anything
 *   the index is unsure about, goes to the filesystem as before. A file
 *   created at runtime is added to the index rather than being contradicted by
 *   it. The index makes the common case fast; it does not get to veto reality.
 *
 * MIT licensed.
 */
#ifndef RVNX_RV_INDEX_H
#define RVNX_RV_INDEX_H

#include <stddef.h>
#include <stdint.h>

/* True when a "noindex.flag" file sits beside the NRO. A zero-rebuild escape
 * hatch: if the index ever disagrees with the card, the user can rename one
 * file and get the previous behaviour back. */
int rv_index_disabled_by_flag(const char *baseDir);

/* Walks dataDir and builds the index. Call once, after the paths resolve and
 * before the engine starts. Returns the number of files indexed, or a negative
 * value if the directory could not be read.
 *
 * Safe to call twice; the second call is a no-op. */
int rv_index_build(const char *dataDir);

void rv_index_shutdown(void);

int rv_index_ready(void);
int rv_index_count(void);

/* Verdict for an absolute path.
 *
 * `size` is best-effort and usually 0: the scan reads d_type rather than
 * stat()ing every entry, because a stat per file is ~2600 extra SD round trips
 * at boot and the size is not needed. A hit still goes to stat_fake, which
 * reports the real one. Do not build anything on this field.
 *
 *    1  indexed
 *    0  definitively absent -- inside the tree, and the scan did not find it
 *   -1  unknown: outside the tree, the index is not ready, or it overflowed
 *
 * Only 0 is worth acting on, and only for a read. Everything else must fall
 * through to the filesystem. */
int rv_index_lookup(const char *path, int64_t *size);

/* A file the engine has just created. Without this, a shader cache written on
 * the first run would be reported absent for the rest of that run. */
void rv_index_note_created(const char *path);

/* A file the engine has just removed. */
void rv_index_note_removed(const char *path);

/* One line of counters for the log. */
void rv_index_stats(char *buffer, size_t capacity);

/* ---- the interception points, wired in imports.c -------------------- */

void  *rv_fopen(const char *path, const char *mode);
int    rv_open(const char *path, int flags, ...);
int    rv_stat(const char *path, void *out);
int    rv_access(const char *path, int mode);
int    rv_remove(const char *path);
int    rv_mkdir(const char *path, unsigned mode);
int    rv_rmdir(const char *path);
int    rv_rename(const char *from, const char *to);

#endif
