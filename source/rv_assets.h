/* rv_assets.h -- detect and cache what game content is actually installed.
 *
 * WHY THIS EXISTS
 * ---------------
 * RVGL needs two separate bodies of content that arrive from different places
 * and that users routinely mix up:
 *
 *   1. the RVGL asset bundle   -- shaders, strings, cups, icons, packs.
 *                                 Ships with RVGL itself.
 *   2. the original game files -- the stock levels, cars, models and wavs.
 *                                 Re-Volt's own data, which you must own.
 *
 * Either one alone gets you a black screen, and the two failures look
 * identical from the outside. Telling them apart is the whole point of this
 * module: "you are missing the game files" and "you are missing the RVGL
 * shaders" are different problems with different fixes, and a user staring at
 * a blank TV cannot distinguish them.
 *
 * WHY IT CACHES
 * -------------
 * A full RVGL install is ~2650 files. Walking that on Switch SD storage costs
 * real seconds every boot for an answer that changes only when the user copies
 * something. So the scan is keyed on a cheap fingerprint -- the mtime and size
 * of a handful of top-level directories -- and the result is written next to
 * the NRO. A matching fingerprint skips the scan entirely; any change to the
 * data directory invalidates it automatically.
 *
 * MIT licensed. Ships no game code and no game assets.
 */
#ifndef RVNX_RV_ASSETS_H
#define RVNX_RV_ASSETS_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
    RV_GROUP_RVGL = 0,   /* RVGL asset bundle    -- required */
    RV_GROUP_GAME,       /* original game files  -- required */
    RV_GROUP_MUSIC,      /* redbook / OST        -- optional */
    RV_GROUP_PACKS,      /* user content packs   -- optional */
    RV_GROUP_COUNT
} RvAssetGroup;

#define RV_MAX_MISSING 6
#define RV_NAME_LEN    48

typedef struct {
    uint8_t present;                              /* every required probe hit */
    uint8_t required;
    uint16_t found;
    uint16_t probes;
    uint16_t missingCount;
    char missing[RV_MAX_MISSING][RV_NAME_LEN];    /* for the error screen */
} RvAssetGroupInfo;

typedef struct {
    RvAssetGroupInfo groups[RV_GROUP_COUNT];

    /* Stock content tallies. A partial count is the signature of a half-copied
     * install, which is much more common than a wholly absent one and is
     * otherwise very hard to diagnose from in-game symptoms. */
    uint16_t stockLevels;      /* of RV_STOCK_LEVEL_COUNT */
    uint16_t stockCars;        /* of RV_STOCK_CAR_COUNT   */

    uint8_t  hasManifest;      /* assets_list.txt present */
    uint8_t  fromCache;        /* answered without touching the content */
    uint64_t fingerprint;
} RvAssetScan;

extern const char *const RV_STOCK_LEVELS[];
extern const int RV_STOCK_LEVEL_COUNT;
extern const char *const RV_STOCK_CARS[];
extern const int RV_STOCK_CAR_COUNT;

/* Scans dataDir, consulting the cache at cachePath first. Pass a NULL
 * cachePath to force a full scan. Returns 1 when everything required is
 * present. */
int rv_assets_scan(const char *dataDir, const char *cachePath, RvAssetScan *out);

/* True when the port can boot: both required groups complete and the frontend
 * level present. */
int rv_assets_ready(const RvAssetScan *scan);

/* Human-readable summary for the log. */
void rv_assets_summarise(const RvAssetScan *scan, char *buffer, size_t capacity);

/* Actionable text for the error screen: what is missing and where to get it. */
void rv_assets_describe_problem(const RvAssetScan *scan, char *buffer, size_t capacity);

/* Deep check against assets_list.txt, the manifest RVGL ships for this very
 * purpose. Slow -- every line is a stat() -- so it is opt-in rather than part
 * of the boot path. Returns the number of listed files that are absent and
 * writes up to `report_max` of their names into `report`. */
int rv_assets_verify_manifest(const char *dataDir, char report[][RV_NAME_LEN],
                              int report_max, int *checked);

/* Drops the cache so the next scan walks the content again. */
void rv_assets_invalidate(const char *cachePath);

#endif
