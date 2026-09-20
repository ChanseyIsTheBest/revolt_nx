/* rv_assets.c -- see rv_assets.h. MIT licensed. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "rv_assets.h"

/* ------------------------------------------------------------------ */
/* What "installed" means                                              */
/* ------------------------------------------------------------------ */

/* The 19 stock Re-Volt levels. Read out of a real install rather than typed
 * from memory, so the spellings are the ones on disk -- muse1 not museum1,
 * toy2 not toys2, markar not marketarena. Getting one wrong understates the
 * tally and sends users hunting for content they already have. */
const char *const RV_STOCK_LEVELS[] = {
    "bot_bat", "frontend", "garden1", "markar", "market1", "market2",
    "muse1", "muse2", "muse_bat", "nhood1", "nhood1_battle", "nhood2",
    "ship1", "ship2", "stunts", "toy2", "toylite", "wild_west1", "wild_west2",
};
const int RV_STOCK_LEVEL_COUNT =
    (int)(sizeof(RV_STOCK_LEVELS) / sizeof(RV_STOCK_LEVELS[0]));

/* Stock cars. Same provenance. Community and RVGL-added cars (bigvolt,
 * bossvolt, tc7-tc12, the jg* set) are deliberately absent: they are not part
 * of the original game data, so counting them would mask a missing purchase. */
const char *const RV_STOCK_CARS[] = {
    "adeon", "amw", "beatall", "candy", "cougar", "dino", "flag", "fone",
    "gencar", "mite", "moss", "mouse", "mud", "panga", "phat", "q", "r5",
    "rc", "rotor", "sgt", "sugo", "tc1", "tc2", "tc3", "tc4", "tc5", "tc6",
    "toyeca",
};
const int RV_STOCK_CAR_COUNT =
    (int)(sizeof(RV_STOCK_CARS) / sizeof(RV_STOCK_CARS[0]));

#define RV_COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* ------------------------------------------------------------------ */

/* Probes that identify each group.
 *
 * Deliberately a handful of representative paths rather than the full
 * manifest: this runs on every boot, and the manifest check
 * (rv_assets_verify_manifest) exists for when a user actually wants the
 * exhaustive answer. Each probe was chosen because it appears in exactly one
 * of the two bundles, which is what lets the two failure modes be told apart.
 */

typedef struct {
    const char *path;
    const char *label;   /* what the user should look for */
} Probe;

/* RVGL's own bundle. None of these exist in the original game data. */
static const Probe kRvglProbes[] = {
    { "shaders",              "shaders/"            },
    { "strings",              "strings/"            },
    { "cups",                 "cups/"               },
    { "gamecontrollerdb.txt", "gamecontrollerdb.txt"},
};

/* The original game files. `levels/frontend` is listed first because it is the
 * menu: without it RVGL starts and has nothing to draw, which is the single
 * most common "it just shows black" report. */
static const Probe kGameProbes[] = {
    { "levels/frontend",      "levels/frontend/ (the menu)" },
    { "levels/nhood1",        "levels/nhood1/"              },
    { "cars/toyeca",          "cars/toyeca/"                },
    { "gfx",                  "gfx/"                        },
    { "models",               "models/"                     },
    { "wavs",                 "wavs/"                       },
};

/* Optional. Their absence costs features, not booting. */
static const Probe kMusicProbes[] = {
    { "redbook",              "redbook/ (soundtrack)" },
};

static const Probe kPackProbes[] = {
    { "packs",                "packs/" },
};

/* Directories whose mtime and size form the fingerprint. Cheap: eight stats
 * against a ~2650-file walk. */
/* Directories RVGL creates for itself on first run. They appear in
 * assets_list.txt because the manifest describes a *populated* install, but
 * their absence on a fresh copy is correct, not a fault. Without this list the
 * manifest check opens with four false positives, which trains people to
 * ignore it. */
static const char *const kRuntimeDirs[] = {
    "cache", "profiles", "replays", "times", "logs", "screenshots",
};

static const char *const kFingerprintDirs[] = {
    "cars", "levels", "gfx", "models", "wavs", "shaders", "strings", "packs",
};

#define RVNX_CACHE_MAGIC   0x43415652u   /* 'RVAC' */
#define RVNX_CACHE_VERSION 2u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t fingerprint;
    RvAssetScan scan;
} CacheFile;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int PathExists(const char *dataDir, const char *relative) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dataDir, relative);
    struct stat info;
    return stat(path, &info) == 0;
}

static uint64_t Fingerprint(const char *dataDir) {
    /* FNV-1a over each directory's mtime and size. Not cryptographic and does
     * not need to be: it only has to change when the user copies something,
     * and a false match costs a stale answer that `rv_assets_invalidate` or a
     * reboot clears. */
    uint64_t hash = 1469598103934665603ULL;

    /* The PATH goes in too. The cache lives beside the NRO while the content
     * it describes does not, so moving or renaming the data folder -- or the
     * port finding a different one than last time -- has to invalidate it.
     * Without this, a cache written when the folder was missing survives the
     * folder appearing under a different name. */
    for (const char *p = dataDir; p != NULL && *p != '\0'; ++p) {
        hash ^= (uint64_t)(unsigned char)*p;
        hash *= 1099511628211ULL;
    }

    for (int i = 0; i < RV_COUNT(kFingerprintDirs); ++i) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s", dataDir, kFingerprintDirs[i]);

        struct stat info;
        uint64_t mtime = 0, size = 0;
        if (stat(path, &info) == 0) {
            mtime = (uint64_t)info.st_mtime;
            size = (uint64_t)info.st_size;
        }

        const uint64_t parts[2] = { mtime, size };
        for (int p = 0; p < 2; ++p) {
            uint64_t value = parts[p];
            for (int b = 0; b < 8; ++b) {
                hash ^= (value >> (b * 8)) & 0xFF;
                hash *= 1099511628211ULL;
            }
        }
    }

    return hash;
}

static void RecordMissing(RvAssetGroupInfo *group, const char *label) {
    if (group->missingCount >= RV_MAX_MISSING) return;
    snprintf(group->missing[group->missingCount], RV_NAME_LEN, "%s", label);
    ++group->missingCount;
}

static void ScanGroup(const char *dataDir, const Probe *probes, int count,
                      int required, RvAssetGroupInfo *group) {
    memset(group, 0, sizeof *group);
    group->probes = (uint16_t)count;
    group->required = (uint8_t)(required != 0);

    for (int i = 0; i < count; ++i) {
        if (PathExists(dataDir, probes[i].path)) {
            ++group->found;
        } else {
            RecordMissing(group, probes[i].label);
        }
    }

    group->present = (group->found == group->probes) ? 1 : 0;
}

static int CountPresent(const char *dataDir, const char *prefix,
                        const char *const *names, int count) {
    int found = 0;
    char relative[256];
    for (int i = 0; i < count; ++i) {
        snprintf(relative, sizeof relative, "%s/%s", prefix, names[i]);
        if (PathExists(dataDir, relative)) ++found;
    }
    return found;
}

/* ------------------------------------------------------------------ */
/* Cache I/O                                                           */
/* ------------------------------------------------------------------ */

static int CacheLoad(const char *cachePath, uint64_t fingerprint, RvAssetScan *out) {
    if (cachePath == NULL) return 0;

    FILE *file = fopen(cachePath, "rb");
    if (file == NULL) return 0;

    CacheFile cache;
    const size_t read = fread(&cache, 1, sizeof cache, file);
    fclose(file);

    if (read != sizeof cache) return 0;
    if (cache.magic != RVNX_CACHE_MAGIC) return 0;
    if (cache.version != RVNX_CACHE_VERSION) return 0;
    if (cache.fingerprint != fingerprint) return 0;

    *out = cache.scan;
    out->fromCache = 1;
    out->fingerprint = fingerprint;
    return 1;
}

static void CacheStore(const char *cachePath, const RvAssetScan *scan) {
    if (cachePath == NULL) return;

    FILE *file = fopen(cachePath, "wb");
    if (file == NULL) return;   /* A read-only card costs speed, not function. */

    CacheFile cache;
    memset(&cache, 0, sizeof cache);
    cache.magic = RVNX_CACHE_MAGIC;
    cache.version = RVNX_CACHE_VERSION;
    cache.fingerprint = scan->fingerprint;
    cache.scan = *scan;
    cache.scan.fromCache = 0;   /* never persist the flag itself */

    fwrite(&cache, 1, sizeof cache, file);
    fclose(file);
}

void rv_assets_invalidate(const char *cachePath) {
    if (cachePath != NULL) remove(cachePath);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int rv_assets_scan(const char *dataDir, const char *cachePath, RvAssetScan *out) {
    if (dataDir == NULL || out == NULL) return 0;

    const uint64_t fingerprint = Fingerprint(dataDir);
    if (CacheLoad(cachePath, fingerprint, out)) return rv_assets_ready(out);

    memset(out, 0, sizeof *out);
    out->fingerprint = fingerprint;

    ScanGroup(dataDir, kRvglProbes,  RV_COUNT(kRvglProbes),  1, &out->groups[RV_GROUP_RVGL]);
    ScanGroup(dataDir, kGameProbes,  RV_COUNT(kGameProbes),  1, &out->groups[RV_GROUP_GAME]);
    ScanGroup(dataDir, kMusicProbes, RV_COUNT(kMusicProbes), 0, &out->groups[RV_GROUP_MUSIC]);
    ScanGroup(dataDir, kPackProbes,  RV_COUNT(kPackProbes),  0, &out->groups[RV_GROUP_PACKS]);

    out->stockLevels = (uint16_t)CountPresent(dataDir, "levels", RV_STOCK_LEVELS,
                                              RV_STOCK_LEVEL_COUNT);
    out->stockCars = (uint16_t)CountPresent(dataDir, "cars", RV_STOCK_CARS,
                                            RV_STOCK_CAR_COUNT);
    out->hasManifest = (uint8_t)PathExists(dataDir, "assets_list.txt");

    CacheStore(cachePath, out);
    return rv_assets_ready(out);
}

int rv_assets_ready(const RvAssetScan *scan) {
    if (scan == NULL) return 0;

    for (int i = 0; i < RV_GROUP_COUNT; ++i) {
        if (scan->groups[i].required && !scan->groups[i].present) return 0;
    }

    /* Belt and braces: the frontend probe is inside the game group, but a
     * partial copy can pass every probe and still be missing most levels.
     * Under half the stock set means something went wrong during the copy. */
    if (scan->stockLevels * 2 < RV_STOCK_LEVEL_COUNT) return 0;

    return 1;
}

void rv_assets_summarise(const RvAssetScan *scan, char *buffer, size_t capacity) {
    if (scan == NULL || buffer == NULL || capacity == 0) return;

    snprintf(buffer, capacity,
             "assets: rvgl=%s game=%s music=%s packs=%s | "
             "levels %u/%d cars %u/%d | manifest=%s | %s",
             scan->groups[RV_GROUP_RVGL].present ? "yes" : "NO",
             scan->groups[RV_GROUP_GAME].present ? "yes" : "NO",
             scan->groups[RV_GROUP_MUSIC].present ? "yes" : "no",
             scan->groups[RV_GROUP_PACKS].present ? "yes" : "no",
             scan->stockLevels, RV_STOCK_LEVEL_COUNT,
             scan->stockCars, RV_STOCK_CAR_COUNT,
             scan->hasManifest ? "yes" : "no",
             scan->fromCache ? "cached" : "scanned");
}

void rv_assets_describe_problem(const RvAssetScan *scan, char *buffer, size_t capacity) {
    if (scan == NULL || buffer == NULL || capacity == 0) return;

    size_t used = 0;
    buffer[0] = '\0';

#define RV_APPEND(...)                                                      \
    do {                                                                    \
        if (used < capacity) {                                              \
            const int n = snprintf(buffer + used, capacity - used, __VA_ARGS__); \
            if (n > 0) used += (size_t)n;                                   \
            if (used > capacity) used = capacity;                           \
        }                                                                   \
    } while (0)

    const RvAssetGroupInfo *rvgl = &scan->groups[RV_GROUP_RVGL];
    const RvAssetGroupInfo *game = &scan->groups[RV_GROUP_GAME];

    if (!rvgl->present) {
        RV_APPEND("Missing RVGL asset bundle:\n");
        for (int i = 0; i < rvgl->missingCount; ++i)
            RV_APPEND("    %s\n", rvgl->missing[i]);
        RV_APPEND("  These ship with RVGL itself.\n"
                  "  Copy your whole RVGL folder, not just the game data.\n\n");
    }

    if (!game->present) {
        RV_APPEND("Missing original game files:\n");
        for (int i = 0; i < game->missingCount; ++i)
            RV_APPEND("    %s\n", game->missing[i]);
        RV_APPEND("  This is Re-Volt's own data. You must own the game.\n"
                  "  This port ships none of it.\n\n");
    }

    if (rvgl->present && game->present &&
        scan->stockLevels * 2 < RV_STOCK_LEVEL_COUNT) {
        RV_APPEND("Install looks incomplete:\n"
                  "    only %u of %d stock levels found\n"
                  "    only %u of %d stock cars found\n\n"
                  "  The copy probably stopped early. Copy again and\n"
                  "  check the card has room.\n\n",
                  scan->stockLevels, RV_STOCK_LEVEL_COUNT,
                  scan->stockCars, RV_STOCK_CAR_COUNT);
    }

    if (used == 0) RV_APPEND("Content looks complete.\n");

#undef RV_APPEND
}

int rv_assets_verify_manifest(const char *dataDir, char report[][RV_NAME_LEN],
                              int report_max, int *checked) {
    if (checked != NULL) *checked = 0;
    if (dataDir == NULL) return -1;

    char manifestPath[512];
    snprintf(manifestPath, sizeof manifestPath, "%s/assets_list.txt", dataDir);

    FILE *file = fopen(manifestPath, "r");
    if (file == NULL) return -1;   /* no manifest: nothing to verify against */

    int missing = 0;
    int tested = 0;
    char line[512];

    while (fgets(line, sizeof line, file) != NULL) {
        /* The manifest is CRLF and lists directories with a trailing slash. */
        size_t length = strlen(line);
        while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r'))
            line[--length] = '\0';
        if (length == 0) continue;
        if (line[length - 1] == '/') line[--length] = '\0';
        if (length == 0) continue;

        int isRuntime = 0;
        for (int i = 0; i < RV_COUNT(kRuntimeDirs); ++i) {
            const size_t n = strlen(kRuntimeDirs[i]);
            if (strncmp(line, kRuntimeDirs[i], n) == 0 &&
                (line[n] == '\0' || line[n] == '/')) { isRuntime = 1; break; }
        }
        if (isRuntime) continue;

        ++tested;
        if (PathExists(dataDir, line)) continue;

        if (missing < report_max && report != NULL) {
            /* Keep the TAIL of a long path: the leading directories are the
             * part every entry shares, so the end is what identifies it. */
            const size_t length2 = strlen(line);
            const size_t keep = (length2 < RV_NAME_LEN - 1) ? length2 : RV_NAME_LEN - 1;
            memcpy(report[missing], line + (length2 - keep), keep);
            report[missing][keep] = '\0';
        }
        ++missing;
    }

    fclose(file);
    if (checked != NULL) *checked = tested;
    return missing;
}
