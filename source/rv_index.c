/* rv_index.c -- see rv_index.h. MIT licensed.
 * Design follows bp_assets.c from the bloonspop port. */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "rv_index.h"
#include "rv_log.h"

/* Sized from a real install. assets_list.txt lists 2655 files, and the tree
 * carries more once per-level and per-car content is counted; 16384 leaves
 * generous room for community packs. Overflow is handled rather than asserted:
 * an overflowed index answers "unknown" to everything and the port behaves as
 * it did before. */
#define RVI_MAX_FILES 16384
#define RVI_SLOTS     32768          /* power of two, > 2x max files */
#define RVI_MAX_DEPTH 12
#define RVI_PATH      512
#define RVI_ARENA     (2 * 1024 * 1024)

typedef struct {
    uint32_t keyOffset;   /* lowercased, '/'-separated key, in the arena */
    int64_t size;         /* < 0 is a tombstone */
    uint8_t isDirectory;
} RvEntry;

static RvEntry *g_entries;
static int32_t *g_slots;
static char *g_arena;
static uint32_t g_arenaUsed;
static int g_count;

static char g_root[RVI_PATH];
static size_t g_rootLength;
static int g_ready;
static int g_overflowed;

/* Counters, for the one line in the log that says whether this was worth it. */
static unsigned g_hits, g_misses, g_unknown;

/* RVGL writes screenshots from its own "PNG Thread" and decodes audio on
 * another, so rv_fopen and the note_* calls arrive from several threads. The
 * hash table grows under those calls; a reader crossing a half-written entry
 * would read a dangling arena offset. The lock is held only around table work,
 * never across the filesystem call it guards. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------ */

static uint32_t Fnv1a(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s != '\0'; ++s) h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

static char Lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* Builds the lookup key: separators normalised, repeats collapsed, lowercased.
 *
 * Lowercasing belongs ONLY in the key. FAT is case-insensitive, so two spellings
 * of a name are the same file and must hash together; but nothing here ever
 * hands a key back to the filesystem, so the case of the real path is never at
 * risk. That distinction is the one bp_assets.c calls out, and it matters on a
 * case-sensitive host. */
static int MakeKey(const char *in, char *out, size_t capacity) {
    size_t o = 0;
    int previousSlash = 0;

    if (in[0] == '.' && (in[1] == '/' || in[1] == '\\')) in += 2;
    while (*in == '/' || *in == '\\') ++in;

    for (; *in != '\0' && o + 1 < capacity; ++in) {
        const char c = (*in == '\\') ? '/' : *in;
        if (c == '/') {
            if (previousSlash) continue;
            previousSlash = 1;
        } else {
            previousSlash = 0;
        }
        out[o++] = Lower(c);
    }

    while (o > 0 && out[o - 1] == '/') --o;
    out[o] = '\0';
    return (int)o;
}

static char *ArenaPut(const char *text, size_t length) {
    if (g_arenaUsed + length + 1 > RVI_ARENA) return NULL;
    char *dest = g_arena + g_arenaUsed;
    memcpy(dest, text, length);
    dest[length] = '\0';
    g_arenaUsed += (uint32_t)(length + 1);
    return dest;
}

static int32_t FindSlot(const char *key) {
    uint32_t slot = Fnv1a(key) & (RVI_SLOTS - 1);
    for (int probe = 0; probe < RVI_SLOTS; ++probe) {
        const int32_t index = g_slots[slot];
        if (index < 0) return -1;
        if (strcmp(g_arena + g_entries[index].keyOffset, key) == 0) return index;
        slot = (slot + 1) & (RVI_SLOTS - 1);
    }
    return -1;
}

static void AddEntry(const char *key, int64_t size, int isDirectory) {
    const int32_t existing = FindSlot(key);
    if (existing >= 0) {
        g_entries[existing].size = size;
        g_entries[existing].isDirectory = (uint8_t)(isDirectory != 0);
        return;
    }

    if (g_count >= RVI_MAX_FILES) { g_overflowed = 1; return; }

    const size_t length = strlen(key);
    char *stored = ArenaPut(key, length);
    if (stored == NULL) { g_overflowed = 1; return; }

    const int index = g_count++;
    g_entries[index].keyOffset = (uint32_t)(stored - g_arena);
    g_entries[index].size = size;
    g_entries[index].isDirectory = (uint8_t)(isDirectory != 0);

    uint32_t slot = Fnv1a(key) & (RVI_SLOTS - 1);
    while (g_slots[slot] >= 0) slot = (slot + 1) & (RVI_SLOTS - 1);
    g_slots[slot] = index;
}

/* ------------------------------------------------------------------ */

static void Scan(const char *absolute, const char *relative, int depth) {
    if (depth > RVI_MAX_DEPTH || g_overflowed) return;

    DIR *dir = opendir(absolute);
    if (dir == NULL) return;

    for (;;) {
        const struct dirent *entry = readdir(dir);
        if (entry == NULL) break;

        const char *name = entry->d_name;
        if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
            continue;

        char childAbsolute[RVI_PATH];
        char childRelative[RVI_PATH];
        if (snprintf(childAbsolute, sizeof childAbsolute, "%s/%s", absolute, name)
                >= (int)sizeof childAbsolute) continue;
        if (snprintf(childRelative, sizeof childRelative, "%s%s%s",
                     relative, relative[0] != '\0' ? "/" : "", name)
                >= (int)sizeof childRelative) continue;

        /* Prefer d_type. A stat per entry is ~2500 extra SD round trips at
         * boot, which is the cost this index exists to avoid; the FAT devoptab
         * fills d_type in, and DT_UNKNOWN falls back. Size is only ever
         * reported through stat_fake on a hit, so not having it is harmless. */
        int isDirectory;
        int64_t size = 0;

#ifdef DT_DIR
        if (entry->d_type == DT_DIR) {
            isDirectory = 1;
        } else if (entry->d_type == DT_REG) {
            isDirectory = 0;
        } else
#endif
        {
            struct stat info;
            if (stat(childAbsolute, &info) != 0) continue;
            if (!S_ISDIR(info.st_mode) && !S_ISREG(info.st_mode)) continue;
            isDirectory = S_ISDIR(info.st_mode);
            size = (int64_t)info.st_size;
        }

        char key[RVI_PATH];
        if (MakeKey(childRelative, key, sizeof key) > 0)
            AddEntry(key, size, isDirectory);

        /* DIRECTORIES ARE INDEXED TOO, and that is not incidental.
         *
         * rv_stat answers "absent" for anything inside the tree that the scan
         * did not record. Indexing only regular files would make every stat of
         * a real directory return ENOENT -- and RVGL stats and opendir()s the
         * level and car directories to enumerate them. An index of files alone
         * does not break slowly; it breaks the menu. */
        if (isDirectory) Scan(childAbsolute, childRelative, depth + 1);

        if (g_overflowed) break;
    }

    closedir(dir);
}

int rv_index_disabled_by_flag(const char *baseDir) {
    if (baseDir == NULL) return 0;
    char path[RVI_PATH];
    snprintf(path, sizeof path, "%s/nopack.flag", baseDir);
    struct stat info;
    return stat(path, &info) == 0;
}

int rv_index_build(const char *dataDir) {
    if (g_ready) return g_count;
    if (dataDir == NULL || *dataDir == '\0') return -1;

    g_entries = (RvEntry *)calloc(RVI_MAX_FILES, sizeof *g_entries);
    g_slots = (int32_t *)malloc(RVI_SLOTS * sizeof *g_slots);
    g_arena = (char *)malloc(RVI_ARENA);
    if (g_entries == NULL || g_slots == NULL || g_arena == NULL) {
        rv_index_shutdown();
        rvnx_log_print(5, "index", "out of memory; running without an index");
        return -1;
    }
    for (int i = 0; i < RVI_SLOTS; ++i) g_slots[i] = -1;

    snprintf(g_root, sizeof g_root, "%s", dataDir);
    /* Trailing separators would break the prefix test in Relative(). */
    g_rootLength = strlen(g_root);
    while (g_rootLength > 0 && g_root[g_rootLength - 1] == '/')
        g_root[--g_rootLength] = '\0';

    const clock_t started = clock();
    Scan(g_root, "", 0);
    const long elapsedMs = (long)((clock() - started) * 1000 / CLOCKS_PER_SEC);

    if (g_count == 0) {
        rv_index_shutdown();
        rvnx_log_print(5, "index", "nothing found under %s; running without an index",
                       dataDir);
        return -1;
    }

    g_ready = 1;

    if (g_overflowed) {
        /* Better to answer "unknown" to everything than to answer "absent"
         * from a partial scan and hide a file that is really there. */
        rvnx_log_print(5, "index",
                       "more than %d files under %s -- index disabled, "
                       "raise RVI_MAX_FILES", RVI_MAX_FILES, dataDir);
        g_ready = 0;
    } else {
        rvnx_log_print(4, "index", "%d files in %ldms, %u KiB arena",
                       g_count, elapsedMs, g_arenaUsed / 1024);
    }

    return g_count;
}

void rv_index_shutdown(void) {
    free(g_entries); g_entries = NULL;
    free(g_slots);   g_slots = NULL;
    free(g_arena);   g_arena = NULL;
    g_count = 0;
    g_arenaUsed = 0;
    g_ready = 0;
    g_overflowed = 0;
}

int rv_index_ready(void) { return g_ready; }
int rv_index_count(void) { return g_count; }

/* Returns the part of `path` below the data root, or NULL when it is not
 * under it. Comparison is case-insensitive because FAT is. */
static const char *Relative(const char *path) {
    if (!g_ready || path == NULL) return NULL;

    for (size_t i = 0; i < g_rootLength; ++i) {
        if (Lower(path[i]) != Lower(g_root[i])) return NULL;
    }
    if (path[g_rootLength] == '\0') return NULL;
    if (path[g_rootLength] != '/' && path[g_rootLength] != '\\') return NULL;

    return path + g_rootLength + 1;
}

int rv_index_lookup(const char *path, int64_t *size) {
    const char *relative = Relative(path);
    if (relative == NULL) { ++g_unknown; return -1; }

    char key[RVI_PATH];
    if (MakeKey(relative, key, sizeof key) <= 0) { ++g_unknown; return -1; }

    pthread_mutex_lock(&g_lock);
    const int32_t index = FindSlot(key);
    const int isTombstone = index >= 0 && g_entries[index].size < 0;
    const int64_t found = index >= 0 ? g_entries[index].size : 0;
    pthread_mutex_unlock(&g_lock);

    if (index < 0 || isTombstone) { ++g_misses; return 0; }
    if (size != NULL) *size = found;
    ++g_hits;
    return 1;
}

void rv_index_note_created(const char *path) {
    const char *relative = Relative(path);
    if (relative == NULL) return;

    char key[RVI_PATH];
    if (MakeKey(relative, key, sizeof key) <= 0) return;

    pthread_mutex_lock(&g_lock);
        struct stat info;
        if (stat(path, &info) != 0) return;
        AddEntry(key, (int64_t)info.st_size, S_ISDIR(info.st_mode));
    pthread_mutex_unlock(&g_lock);
}

void rv_index_note_removed(const char *path) {
    const char *relative = Relative(path);
    if (relative == NULL) return;

    char key[RVI_PATH];
    if (MakeKey(relative, key, sizeof key) <= 0) return;

    pthread_mutex_lock(&g_lock);
        const int32_t index = FindSlot(key);
        if (index >= 0) g_entries[index].size = -1;   /* tombstone; see below */
    pthread_mutex_unlock(&g_lock);
}

void rv_index_stats(char *buffer, size_t capacity) {
    if (buffer == NULL || capacity == 0) return;
    snprintf(buffer, capacity,
             "index: %s, %d files | %u hits, %u misses avoided, %u outside",
             g_ready ? "on" : "off", g_count, g_hits, g_misses, g_unknown);
}

/* ------------------------------------------------------------------ */
/* Interception                                                        */
/* ------------------------------------------------------------------ */

/* libc_shim.c's versions, which do the bionic translation. We sit in front of
 * them and answer only the case they would spend a directory walk on.
 *
 * Declared with the same types libc_shim.h uses. An opaque `void *` here would
 * link fine and then disagree silently if either side ever changed shape. */
struct bionic_stat;

FILE *fopen_fake(const char *path, const char *mode);
int   open_fake(const char *path, int flags, ...);
int   stat_fake(const char *path, struct bionic_stat *st);
int   mkdir_fake(const char *path, unsigned mode);

static int IsReadOnlyMode(const char *mode) {
    if (mode == NULL) return 0;
    if (mode[0] != 'r') return 0;
    for (const char *p = mode; *p != '\0'; ++p)
        if (*p == '+') return 0;      /* r+ can create nothing, but may write */
    return 1;
}

static int IsWriteFlags(int flags) {
    /* From the headers rather than hardcoded: O_CREAT is 0x200 on newlib and
     * 0x40 on Linux, so a literal would be wrong on the host this is tested on. */
    return (flags & O_ACCMODE) != O_RDONLY || (flags & O_CREAT) != 0;
}

void *rv_fopen(const char *path, const char *mode) {
    if (IsReadOnlyMode(mode) && rv_index_lookup(path, NULL) == 0) {
        /* Inside the tree and not in it. No filesystem call at all. */
        errno = ENOENT;
        return NULL;
    }

    FILE *file = fopen_fake(path, mode);
    if (file != NULL && !IsReadOnlyMode(mode)) rv_index_note_created(path);
    return file;
}

int rv_open(const char *path, int flags, ...) {
    if (!IsWriteFlags(flags) && rv_index_lookup(path, NULL) == 0) {
        errno = ENOENT;
        return -1;
    }

    /* The mode argument only exists when O_CREAT is set. Reading it
     * unconditionally is undefined behaviour and would hand open_fake a
     * garbage mode on every plain read. */
    int fd;
    if ((flags & O_CREAT) != 0) {
        va_list args;
        va_start(args, flags);
        const int mode = va_arg(args, int);
        va_end(args);
        fd = open_fake(path, flags, mode);
    } else {
        fd = open_fake(path, flags);
    }

    if (fd >= 0 && IsWriteFlags(flags)) rv_index_note_created(path);
    return fd;
}

int rv_stat(const char *path, void *out) {
    if (rv_index_lookup(path, NULL) == 0) { errno = ENOENT; return -1; }
    return stat_fake(path, (struct bionic_stat *)out);
}

int rv_access(const char *path, int mode) {
    if (rv_index_lookup(path, NULL) == 0) { errno = ENOENT; return -1; }
    return access(path, mode);
}

/* RVGL imports mkdir, rename, rmdir and remove. Every one of them can make
 * the index disagree with the card, and a stale "absent" is far worse than no
 * index -- it hides a file that is really there. These four are the complete
 * set for this engine; it imports no unlink, creat or openat. */

int rv_mkdir(const char *path, unsigned mode) {
    const int result = mkdir_fake(path, mode);
    if (result == 0) rv_index_note_created(path);
    return result;
}

int rv_rmdir(const char *path) {
    const int result = rmdir(path);
    if (result == 0) rv_index_note_removed(path);
    return result;
}

int rv_rename(const char *from, const char *to) {
    const int result = rename(from, to);
    if (result == 0) {
        /* Order matters if a path renames onto itself. */
        rv_index_note_created(to);
        rv_index_note_removed(from);
    }
    return result;
}

int rv_remove(const char *path) {
    const int result = remove(path);
    if (result == 0) rv_index_note_removed(path);
    return result;
}
