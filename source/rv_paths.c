/* rv_paths.c -- see rv_paths.h. MIT licensed. */

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "config.h"
#include "rv_assets.h"
#include "rv_log.h"
#include "rv_paths.h"

static RvPaths g_paths;

/* The .so files the loader maps. libSDL2 and libSDL2_image are deliberately
 * absent: switch-sdl2 provides both natively, and the Android builds drag in
 * libandroid.so, libGLESv1_CM.so and libOpenSLES.so with them. */
static const char *const kRequiredLibs[] = {
    "libmain.so", "libopenal.so", "libmpg123.so", "libsndfile.so", "libunistring.so",
};

#define RV_LIB_COUNT ((int)(sizeof(kRequiredLibs) / sizeof(kRequiredLibs[0])))

/* Searched in order when argv[0] is unusable. The first is where earlier
 * versions insisted on living, so an existing install keeps working. */
/* Directories RVGL writes into and does not create for itself.
 *
 * Read out of the binary's string table: profiles/rvgl_log.txt is the very
 * first file it opens, and on FAT an fopen for write into a directory that
 * does not exist simply fails. libc_shim's mkdir_p only auto-creates parents
 * for ABSOLUTE paths under sj_home(), and RVGL passes these relative to the
 * working directory, so nothing was creating them.
 *
 * cache/shaders is nested, so cache must come first. */
static const char *const kRuntimeDirs[] = {
    "profiles", "replays", "times", "packs", "cache", "cache/shaders",
};

#define RV_RUNTIME_COUNT ((int)(sizeof(kRuntimeDirs) / sizeof(kRuntimeDirs[0])))

static const char *const kFallbackRoots[] = {
    "sdmc:/switch/revoltnx", "sdmc:/switch/rvgl", "sdmc:/rvgl", "sdmc:/switch",
};

#define RV_FALLBACK_COUNT ((int)(sizeof(kFallbackRoots) / sizeof(kFallbackRoots[0])))

/* Searched one level deep when none of the names above match, looking for a
 * directory that holds libmain.so. */
static const char *const kSearchRoots[] = { "sdmc:/switch", "sdmc:/" };

#define RV_SEARCH_COUNT ((int)(sizeof(kSearchRoots) / sizeof(kSearchRoots[0])))

static int Exists(const char *path) {
    struct stat info;
    return stat(path, &info) == 0;
}

static int IsDirectory(const char *path) {
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

static void Join(char *out, size_t capacity, const char *dir, const char *leaf) {
    if (dir == NULL || *dir == '\0') { snprintf(out, capacity, "%s", leaf); return; }
    const size_t length = strlen(dir);
    const char *separator = (length > 0 && dir[length - 1] == '/') ? "" : "/";
    snprintf(out, capacity, "%s%s%s", dir, separator, leaf);
}

size_t rv_paths_dirname(const char *path, char *out, size_t capacity) {
    if (path == NULL || out == NULL || capacity == 0) return 0;

    const char *lastSlash = strrchr(path, '/');
    if (lastSlash == NULL) {
        /* A bare filename: the launcher gave us no directory at all. */
        out[0] = '\0';
        return 0;
    }

    /* Keep the slash when dropping it would change the meaning --
     * "sdmc:/x.nro" must become "sdmc:/", not "sdmc:". */
    size_t length = (size_t)(lastSlash - path);
    if (length == 0 || path[length - 1] == ':') length += 1;

    if (length >= capacity) return 0;
    memcpy(out, path, length);
    out[length] = '\0';
    return length;
}

/* libmain.so alone is the test here: the rest are checked later, by a pass
 * that names each missing one individually. */
static int LooksLikeLibDir(const char *dir) {
    char path[512];
    Join(path, sizeof path, dir, "libmain.so");
    return Exists(path);
}

/* `cars` and `levels` together are specific enough that nothing else on a
 * Switch card will match by accident. */
static int LooksLikeDataDir(const char *dir) {
    char cars[512], levels[512];
    Join(cars, sizeof cars, dir, "cars");
    Join(levels, sizeof levels, dir, "levels");
    return IsDirectory(cars) && IsDirectory(levels);
}

/* Subfolder names worth trying before scanning. Ordered by how likely they
 * are, so the common cases cost two stat calls rather than a directory walk. */
static const char *const kDataFolderNames[] = {
    "rvgl", "assets", "data", "gamedata", "game", "RVGL", "Assets",
};

/* Finds the RVGL content under `base`.
 *
 * The named list is a shortcut, not the rule. Anyone can call the folder
 * anything, so if none of the names match we walk one level of subdirectories
 * and take whichever actually contains cars/ and levels/. That is what makes
 * "put it wherever you like" true rather than "put it in one of seven places
 * I thought of".
 *
 * Returns 1 and fills `out` on success. */
static int FindDataDir(const char *base, char *out, size_t capacity) {
    char candidate[448];

    for (int i = 0; i < (int)(sizeof kDataFolderNames / sizeof kDataFolderNames[0]); ++i) {
        Join(candidate, sizeof candidate, base, kDataFolderNames[i]);
        if (LooksLikeDataDir(candidate)) {
            snprintf(out, capacity, "%s", candidate);
            return 1;
        }
    }

    if (LooksLikeDataDir(base)) {
        snprintf(out, capacity, "%s", base);
        return 1;
    }

    DIR *dir = opendir(base);
    if (dir == NULL) return 0;

    int found = 0;
    while (!found) {
        const struct dirent *entry = readdir(dir);
        if (entry == NULL) break;

        const char *name = entry->d_name;
        if (name[0] == '.') continue;   /* . .. and launcher dotfiles */

        Join(candidate, sizeof candidate, base, name);
        if (!IsDirectory(candidate)) continue;
        if (!LooksLikeDataDir(candidate)) continue;

        snprintf(out, capacity, "%s", candidate);
        found = 1;
    }

    closedir(dir);
    return found;
}

/* Fills lib/data/log/cache from an assumed base. Returns non-zero when the
 * base holds something recognisable, so the caller can keep looking. */
static int PopulateFrom(const char *base, RvPaths *out) {
    snprintf(out->base, sizeof out->base, "%s", base);

    /* Sized to match RvPaths::lib and ::data so the copies below cannot
     * truncate -- the compiler checks this. */
    char candidate[448];

    Join(candidate, sizeof candidate, base, "lib");
    if (LooksLikeLibDir(candidate)) {
        snprintf(out->lib, sizeof out->lib, "%s", candidate);
        out->libBesideNro = 0;
    } else if (LooksLikeLibDir(base)) {
        snprintf(out->lib, sizeof out->lib, "%s", base);
        out->libBesideNro = 1;
    } else {
        /* Nothing there yet. Point at the conventional spot so the error
         * screen can tell the user where to put them. */
        Join(out->lib, sizeof out->lib, base, "lib");
        out->libBesideNro = 0;
    }

    if (FindDataDir(base, out->data, sizeof out->data)) {
        out->inPlace = (strcmp(out->data, base) == 0);
    } else {
        /* Nothing yet. Name the conventional spot so the error screen has
         * somewhere concrete to point at. */
        Join(out->data, sizeof out->data, base, "rvgl");
        out->inPlace = 0;
    }

    /* Our own files sit beside the NRO either way. In an in-place install that
     * puts them alongside RVGL's own, which is where someone poking at the
     * folder would look for them. */
    Join(out->log, sizeof out->log, base, "revoltnx.log");
    Join(out->cache, sizeof out->cache, base, "assetcache.bin");
    Join(out->userDb, sizeof out->userDb, base, "gamecontrollerdb_switch.txt");

    return LooksLikeLibDir(out->lib) || LooksLikeDataDir(out->data);
}

int rv_paths_resolve(int argc, char *const argv[]) {
    memset(&g_paths, 0, sizeof g_paths);

    /* argv[0] is the NRO's own path under hbmenu, and it is the right answer
     * whenever it exists because it relies on no convention at all. */
    if (argc > 0 && argv != NULL && argv[0] != NULL && argv[0][0] != '\0') {
        char dir[384];
        if (rv_paths_dirname(argv[0], dir, sizeof dir) > 0 && IsDirectory(dir)) {
            g_paths.fromArgv = 1;
            if (PopulateFrom(dir, &g_paths)) return 1;
            /* Real directory, nothing recognisable in it. Keep it as the base:
             * it is where the user put the NRO, so it is where the error
             * screen should tell them to put the rest. */
        }
    }

    for (int i = 0; i < RV_FALLBACK_COUNT; ++i) {
        if (!IsDirectory(kFallbackRoots[i])) continue;

        RvPaths candidate;
        memset(&candidate, 0, sizeof candidate);
        if (PopulateFrom(kFallbackRoots[i], &candidate)) {
            g_paths = candidate;   /* fromArgv stays 0: this came from a guess */
            return 1;
        }
    }

    /* Still nothing, so stop guessing names and go looking. One level under
     * each search root, take the first directory holding libmain.so. This is
     * what makes a launcher that supplies no argv[0] survive a folder called
     * anything at all -- "revolt_nx", "RVGL-Switch", whatever the user chose. */
    for (int i = 0; i < RV_SEARCH_COUNT; ++i) {
        DIR *dir = opendir(kSearchRoots[i]);
        if (dir == NULL) continue;

        char found[384] = {0};
        const struct dirent *entry;
        while (found[0] == '\0' && (entry = readdir(dir)) != NULL) {
            if (entry->d_name[0] == '.') continue;

            char candidatePath[384];
            Join(candidatePath, sizeof candidatePath, kSearchRoots[i], entry->d_name);
            if (!IsDirectory(candidatePath)) continue;
            if (!LooksLikeLibDir(candidatePath)) {
                char nested[448];
                Join(nested, sizeof nested, candidatePath, "lib");
                if (!LooksLikeLibDir(nested)) continue;
            }
            snprintf(found, sizeof found, "%s", candidatePath);
        }
        closedir(dir);

        if (found[0] != '\0') {
            RvPaths candidate;
            memset(&candidate, 0, sizeof candidate);
            if (PopulateFrom(found, &candidate)) {
                g_paths = candidate;
                return 1;
            }
        }
    }

    if (g_paths.base[0] == '\0') PopulateFrom(kFallbackRoots[0], &g_paths);
    return 0;
}

const RvPaths *rv_paths(void) { return &g_paths; }
const char *rv_paths_data(void) { return g_paths.data; }
const char *rv_paths_lib(void) { return g_paths.lib; }

int rv_paths_prepare(char *missing, unsigned missing_len, RvAssetScan *out_scan) {
    if (missing != NULL && missing_len > 0) missing[0] = '\0';

    mkdir(g_paths.base, 0777);
    if (!g_paths.libBesideNro) mkdir(g_paths.lib, 0777);
    if (!g_paths.inPlace) mkdir(g_paths.data, 0777);

    rvnx_log_print(4, "paths", "base %s (%s)", g_paths.base,
                   g_paths.fromArgv ? "from argv[0]" : "fallback");
    rvnx_log_print(4, "paths", "lib  %s%s", g_paths.lib,
                   g_paths.libBesideNro ? " (beside the NRO)" : "");
    rvnx_log_print(4, "paths", "data %s%s", g_paths.data,
                   g_paths.inPlace ? " (in-place install)" : "");

    /* People copy the whole lib/arm64-v8a/ folder across. Harmless, but say so
     * rather than leave them wondering why two files are apparently ignored. */
    char extra[512];
    Join(extra, sizeof extra, g_paths.lib, "libSDL2.so");
    if (Exists(extra))
        rvnx_log_print(4, "paths",
                       "libSDL2.so/libSDL2_image.so present and ignored -- "
                       "this port uses the Switch's own SDL2");

    /* Before anything opens a file. RVGL resolves these against the working
     * directory, which main.c sets to the data directory. */
    char runtime[512];
    for (int i = 0; i < RV_RUNTIME_COUNT; ++i) {
        Join(runtime, sizeof runtime, g_paths.data, kRuntimeDirs[i]);
        mkdir(runtime, 0777);
    }

    int ok = 1;
    char path[512];

    for (int i = 0; i < RV_LIB_COUNT; ++i) {
        Join(path, sizeof path, g_paths.lib, kRequiredLibs[i]);
        if (Exists(path)) continue;

        ok = 0;
        if (missing != NULL) {
            const unsigned used = (unsigned)strlen(missing);
            if (used + 2 < missing_len)
                snprintf(missing + used, missing_len - used, "  %s\n", kRequiredLibs[i]);
        }
    }

    if (!ok && missing != NULL) {
        const unsigned used = (unsigned)strlen(missing);
        if (used < missing_len)
            snprintf(missing + used, missing_len - used,
                     "  ...expected in %s\n\n", g_paths.lib);
    }

    RvAssetScan scan;
    if (!rv_assets_scan(g_paths.data, g_paths.cache, &scan)) {
        ok = 0;
        if (missing != NULL) {
            const unsigned used = (unsigned)strlen(missing);
            if (used < missing_len)
                rv_assets_describe_problem(&scan, missing + used, missing_len - used);
        }
    }

    if (out_scan != NULL) *out_scan = scan;
    return ok;
}
