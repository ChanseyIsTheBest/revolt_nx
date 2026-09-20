/* rv_paths.h -- work out where we are, rather than being told.
 *
 * The port used to insist on sdmc:/switch/revoltnx. It now locates itself from
 * argv[0], which hbmenu sets to the full path of the NRO it launched, so the
 * folder can be called anything and live anywhere on the card.
 *
 * Two layouts are recognised, because both are things people actually do:
 *
 *   SUBFOLDER              IN-PLACE
 *   <dir>/revoltnx.nro     <dir>/revoltnx.nro
 *   <dir>/lib/  (the .so)  <dir>/lib/  (the .so)
 *   <dir>/rvgl/cars/       <dir>/cars/
 *   <dir>/rvgl/levels/     <dir>/levels/
 *
 * In-place matters: the obvious thing to do with this NRO is drop it straight
 * into an RVGL folder you already have, and that should simply work rather
 * than requiring the whole install be moved down a level.
 *
 * The .so files are also accepted directly beside the NRO, for people who
 * would rather not have a lib/ subfolder.
 *
 * MIT licensed.
 */
#ifndef RVNX_RV_PATHS_H
#define RVNX_RV_PATHS_H

#include <stddef.h>

#include "rv_assets.h"

typedef struct {
    char base[384];    /* the directory holding the NRO */
    char lib[448];     /* where the Android .so files were found */
    char data[448];    /* the RVGL install root */
    char log[512];
    char cache[512];
    char userDb[512];  /* optional gamecontrollerdb_switch.txt */

    int inPlace;       /* data == base: the NRO sits inside an RVGL install */
    int libBesideNro;  /* .so files are in base rather than base/lib */
    int fromArgv;      /* base came from argv[0], not a fallback */
} RvPaths;

/* Call once, first thing in main. Returns 0 if nothing plausible was found,
 * in which case the resolved paths still hold the best guess so the error
 * screen can name it. */
int rv_paths_resolve(int argc, char *const argv[]);

const RvPaths *rv_paths(void);

/* Creates the directory tree and checks the user supplied everything.
 * `missing` receives text suitable for the error screen. */
int rv_paths_prepare(char *missing, unsigned missing_len, RvAssetScan *out_scan);

const char *rv_paths_data(void);
const char *rv_paths_lib(void);

/* Strips the last path component. Exposed for testing: argv[0] arrives in
 * several shapes depending on the launcher and this is where that is handled. */
size_t rv_paths_dirname(const char *path, char *out, size_t capacity);

#endif
