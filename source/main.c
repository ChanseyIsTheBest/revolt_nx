/* main.c -- revoltnx entry point.
 *
 * MIT licensed. Ships no game code and no game assets.
 *
 * WHAT SHAPE OF PORT THIS IS
 * --------------------------
 * RVGL's Android build is a plain SDL2 application with a thin Java shell, not
 * a GLSurfaceView or NativeActivity engine. That makes this port far smaller
 * than the reference ports it is built from, and the difference is worth
 * stating because it drives every decision below.
 *
 * Read out of the supplied libmain.so rather than assumed:
 *
 *   - it EXPORTS SDL_main. On Android, SDLActivity's Java layer calls that
 *     after setting up the GL surface. Here we call it directly and it never
 *     returns until the game quits, so this file has no frame loop: SDL2 and
 *     the engine own the loop between them.
 *   - it has NO Java_* export, no JNI_OnLoad, and imports no JNIEnv function.
 *     The supplied classes.dex is SDLActivity, which switch-sdl2 replaces
 *     wholesale. There is no JNI bridge in this tree and none is needed.
 *   - its only Android-specific SDL imports are SDL_AndroidGetExternalStoragePath
 *     and SDL_AndroidRequestPermission. Both are in rv_sdl_stubs.c.
 *   - it imports no GL or EGL symbol at all. glad is statically linked inside
 *     it and resolves through SDL_GL_GetProcAddress at runtime, so it binds to
 *     whatever context SDL2 gives it. Nothing here has to hand it a driver.
 *   - ENet, Vorbis and FLAC are statically linked in too, which is why the
 *     socket calls (accept/bind/connect/getaddrinfo) appear directly in its
 *     import list and are answered by compat_stubs.c.
 *
 * WHICH LIBRARIES ARE LOADED AND WHICH ARE REPLACED
 * -------------------------------------------------
 * Five of the seven Android libraries are mapped by so_util:
 *
 *     libunistring.so   libc only
 *     libmpg123.so      libc only
 *     libsndfile.so     libc only
 *     libopenal.so      libc + OpenSL ES -> opensles.c, over SDL2 audio
 *     libmain.so        the engine
 *
 * libSDL2.so and libSDL2_image.so are NOT loaded. switch-sdl2 provides both
 * natively, with real Switch windowing, HID and audio. Loading the Android
 * builds instead would mean answering for libandroid.so, libGLESv1_CM.so and
 * libOpenSLES.so as well, to get a strictly worse SDL.
 *
 * LOAD ORDER MATTERS
 * ------------------
 * Modules are mapped in dependency order and every module's init array runs
 * before the next one loads. libmain.so is last because its constructors call
 * into OpenAL and mpg123, which have to be initialised by then.
 */

#include <switch.h>

#include <SDL2/SDL.h>

#include <malloc.h>     /* memalign: newlib puts it here, not in stdlib.h */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "so_util.h"
#include "util.h"
#include "error.h"
#include "imports.h"
#include "rv_paths.h"
#include "rv_assets.h"
#include "pps_stdout.h"
#include "pps_jni.h"
#include "compat_stubs.h"
#include "rv_log.h"
#include "opensles.h"
#include "rv_net.h"
#include "rv_input.h"
#include "rv_video.h"
#include "rv_cursor.h"
#include "asset_pack.h"
#include "rv_ramcache.h"

/* ------------------------------------------------------------------ */
/* libnx configuration                                                 */
/* ------------------------------------------------------------------ */

/* Applet mode does not have the memory to map ~40 MB of modules plus the
 * engine's own heap. This must run as a game override (hold R while launching
 * a game from the home menu). Requesting AppletType_Application here is what
 * makes the full heap available. */
u32 __nx_applet_type = AppletType_Application;

/* The engine faults inside its own code on a bad pointer; a exception stack
 * means we get a register dump in the log instead of a silent reboot. */
u8 __nx_exception_stack[0x4000] __attribute__((aligned(16)));
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

/* ------------------------------------------------------------------ */
/* Modules                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *file;
    size_t      reserve;
    so_module   module;
    uint8_t    *area;
    int         loaded;
} RvModule;

/* Order is load order. Do not reorder without reading the note above. */
static RvModule g_modules[] = {
    { "libunistring.so", RVNX_UNISTRING_LOAD_SIZE, {0}, NULL, 0 },
    { "libmpg123.so",    RVNX_MPG123_LOAD_SIZE,    {0}, NULL, 0 },
    { "libsndfile.so",   RVNX_SNDFILE_LOAD_SIZE,   {0}, NULL, 0 },
    { "libopenal.so",    RVNX_OPENAL_LOAD_SIZE,    {0}, NULL, 0 },
    { "libmain.so",      RVNX_MAIN_LOAD_SIZE,      {0}, NULL, 0 },
};

#define RV_MODULE_COUNT (sizeof(g_modules) / sizeof(*g_modules))

/* The engine module, for SDL_main lookup. Always the last entry. */
#define RV_ENGINE (&g_modules[RV_MODULE_COUNT - 1])

/* Bionic TLS for the main thread.
 *
 * The engine's stack-protector prologues read their canary from
 * TPIDR_EL0+0x28. libc_shim's pthread_create wrapper installs a block for
 * every thread the engine spawns; this is the one for the thread that calls
 * SDL_main. Sharing a single block across threads corrupts the guard slot,
 * which presents as a random __stack_chk_fail long after the real fault. */
static uint8_t g_main_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));

/* ------------------------------------------------------------------ */

typedef int (*fn_sdl_main)(int argc, char **argv);

/* A zero-rebuild escape hatch. The pack sits in front of every file read, so
 * if it ever disagrees with the card the user needs a way out that does not
 * involve a toolchain. */
static int PackDisabledByFlag(const char *baseDir) {
    if (baseDir == NULL) return 0;
    char path[512];
    snprintf(path, sizeof path, "%s/nopack.flag", baseDir);
    struct stat info;
    return stat(path, &info) == 0;
}

/* How much address space a module's LOAD segments actually span.
 *
 * Read from the file rather than guessed from its size, because for this
 * library those two numbers are three orders of magnitude apart: libmain.so is
 * a 2.9 MB file whose second LOAD segment carries a 167.9 MiB .bss, for a
 * 170.8 MiB memory image. A reservation sized from the file is short by a
 * factor of fifty and so_load fails with nothing to indicate why.
 *
 * Computing it here also means RVGL can grow its static allocations without
 * this port needing to be re-tuned. Returns 0 if the file is unreadable or is
 * not a 64-bit little-endian ELF. */
static size_t RequiredLoadSize(const char *path) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) return 0;

    unsigned char header[64];
    if (fread(header, 1, sizeof header, file) != sizeof header) {
        fclose(file);
        return 0;
    }

    if (memcmp(header, "\177ELF", 4) != 0 || header[4] != 2 /* ELFCLASS64 */ ||
        header[5] != 1 /* ELFDATA2LSB */) {
        fclose(file);
        return 0;
    }

    uint64_t phoff;
    uint16_t phentsize, phnum;
    memcpy(&phoff, header + 32, sizeof phoff);
    memcpy(&phentsize, header + 54, sizeof phentsize);
    memcpy(&phnum, header + 56, sizeof phnum);

    if (phentsize < 56 || phnum == 0 || phnum > 128) {
        fclose(file);
        return 0;
    }

    uint64_t end = 0;
    for (uint16_t i = 0; i < phnum; ++i) {
        unsigned char entry[56];
        if (fseek(file, (long)(phoff + (uint64_t)i * phentsize), SEEK_SET) != 0) break;
        if (fread(entry, 1, sizeof entry, file) != sizeof entry) break;

        uint32_t type;
        uint64_t vaddr, memsz;
        memcpy(&type, entry + 0, sizeof type);
        memcpy(&vaddr, entry + 16, sizeof vaddr);
        memcpy(&memsz, entry + 40, sizeof memsz);

        if (type != 1 /* PT_LOAD */) continue;
        if (vaddr + memsz > end) end = vaddr + memsz;
    }

    fclose(file);
    return (size_t)end;
}

static void ShowMissingFiles(const char *missing) {
    char message[2048];
    snprintf(message, sizeof message,
             "revoltnx: cannot start.\n\n"
             "%s\n"
             "Looking beside the NRO, in:\n"
             "  %s\n\n"
             "Either put lib/ and rvgl/ there, or drop the NRO\n"
             "straight into an RVGL folder you already have --\n"
             "both layouts work.\n\n"
             "The libraries come out of the RVGL APK; unzip it,\n"
             "they are in lib/arm64-v8a/. Do NOT copy libSDL2.so\n"
             "or libSDL2_image.so: this port uses the Switch's\n"
             "own SDL2.\n\n"
             "This port ships no game code and no game data.\n",
             missing, rv_paths()->base);
    error_screen(message);
}

static int LoadModule(RvModule *m) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", rv_paths_lib(), m->file);

    char status[128];
    snprintf(status, sizeof status, "loading %s", m->file);
    startup_status_update(status);


    const size_t required = RequiredLoadSize(path);
    if (required == 0)
        fatal_error("%s is not a 64-bit little-endian ELF.\n\n"
                    "Is it the arm64-v8a build? The armeabi-v7a one will not work.",
                    m->file);

    /* Round up to a page and add a little slack for so_util's own bookkeeping,
     * then take whichever is larger: what the file needs, or the configured
     * floor. */
    size_t reserve = (required + RVNX_LOAD_SLACK + 0xFFFFu) & ~(size_t)0xFFFFu;
    if (reserve < m->reserve) reserve = m->reserve;
    m->reserve = reserve;

    rvnx_log_print(4, "loader", "%s: image %zu KiB, reserving %zu KiB",
                   m->file, required / 1024, reserve / 1024);

    m->area = memalign(0x1000, reserve);
    if (m->area == NULL)
        fatal_error("Out of memory: %s needs %zu MiB of address space\n"
                    "(a %zu MiB file with a large .bss) and the reservation\n"
                    "failed.\n\n"
                    "This must run as a title override -- hold R while starting\n"
                    "an installed game. Applet mode does not have the heap.",
                    m->file, reserve / (1024 * 1024),
                    required / (1024 * 1024));

    if (so_load(&m->module, path, m->area, reserve) < 0)
        fatal_error("could not load %s\n\n"
                    "Needed %zu MiB, reserved %zu MiB.\n"
                    "Is it the arm64-v8a build?",
                    m->file, required / (1024 * 1024), reserve / (1024 * 1024));

    if (so_relocate(&m->module) < 0)
        fatal_error("relocation failed for %s", m->file);

    /* taint_missing_imports = 1: an unresolved symbol becomes a trap that
     * names itself when called, rather than a null jump. These libraries are
     * BIND_NOW, so most gaps surface here at load time anyway. */
    if (so_resolve(&m->module, dynlib_functions, (int)dynlib_numfunctions, 1) < 0)
        fatal_error("unresolved imports in %s\n\nRun tools/verify_imports.py.", m->file);

    /* ORDER MATTERS HERE, and getting it wrong is not a subtle failure.
     *
     * so_load only RESERVES the virtual range. A reservation is bookkeeping;
     * nothing is mapped at load_virtbase yet. so_finalize is what performs the
     * svcMapProcessCodeMemory that aliases load_base to load_virtbase and sets
     * the page permissions.
     *
     * So so_flush_caches, which touches load_virtbase directly, MUST come
     * after so_finalize. This file had them the other way round and the first
     * hardware run died on it -- so_util.c catches the mistake by name rather
     * than faulting inside armDCacheFlush with nothing pointing back here,
     * which is the only reason it was a one-line fix.
     *
     * The same trap applies to anything that patches .text: it has to run
     * BEFORE so_finalize, while load_base is still ordinary writable memory.
     * The kernel never permits a W->X transition on code memory once mapped,
     * so a patch applied afterwards either faults or silently does nothing.
     * Nothing here patches .text today; that is where it would go.
     *
     * so_patch_stack_canaries is deliberately not called. The engine's
     * prologues read their guard from TPIDR_EL0+0x28, which install_bionic_tls
     * already provides, and pattern-matching branch sites risks rewriting
     * branches that are not canary checks at all. */
    so_finalize(&m->module);
    so_flush_caches(&m->module);

    /* Static initialisers run real engine code. Everything the shims provide
     * has to be up before this line -- which is why SDL, the socket layer and
     * the controller mappings are all initialised in main() before the first
     * module is loaded. */
    so_execute_init_array(&m->module);
    so_free_temp(&m->module);

    m->loaded = 1;
    return 0;
}

int main(int argc, char *argv[]) {
    /* First, before anything can want a path. hbmenu passes the NRO's own
     * location in argv[0], so the install folder can be named anything and
     * live anywhere. See rv_paths.h for the layouts recognised. */
    rv_paths_resolve(argc, argv);

    /* Console first: everything below can fail, and a message on the TV beats
     * a black screen. error.c tears this down before the game starts. */
    consoleInit(NULL);
    pps_stdout_to_console();

    /* ENet is statically linked into libmain.so, so the socket calls appear
     * directly in its import table. rv_net brings up BSD with a session count
     * and buffer sizes sized for it, plus nifm for link status. */
    rv_net_init();

    char missing[1280];
    RvAssetScan assets;
    if (!rv_paths_prepare(missing, sizeof missing, &assets)) {
        ShowMissingFiles(missing);
        rv_net_exit();
        consoleExit(NULL);
        return 0;
    }

    rvnx_stdout_init(rv_paths()->log);
    startup_status_begin("revoltnx starting");

    /* Walk the data tree once so that the thousands of probes RVGL makes
     * before it reaches the menu -- most of them expected misses -- cost a
     * memory read instead of a FAT32 directory walk. See rv_index.h. */
    /* One file instead of thousands.
     *
     * FAT32 over the Switch's SD interface charges a directory walk per open,
     * and RVGL issues several thousand before it reaches the menu. The pack
     * concatenates everything under the data directory into assets.nxpack with
     * a sorted index beside it, so an open becomes a binary search and a seek.
     *
     * libc_shim.c already consults asset_pack at every file entry point, so
     * nothing else has to change: opening the pack here is the whole wiring.
     * Runtime directories are excluded at build time -- see kNeverPack -- or
     * the read-only pack would shadow every save.
     *
     * Building is a one-off that takes minutes on first run. nopack.flag
     * beside the NRO skips all of this. */
    rv_ramcache_configure(rv_paths()->base);

    if (PackDisabledByFlag(rv_paths()->base)) {
        rvnx_log_print(4, "pack", "nopack.flag present -- reading files directly");
    } else if (asset_pack_open_existing(rv_paths()->base)) {
        rvnx_log_print(4, "pack", "assets.nxpack open, %zu entries",
                       asset_pack_entry_count());
    } else {
        startup_status_update("building asset pack (first run, please wait)");
        rvnx_log_print(4, "pack", "no pack yet: %s", asset_pack_error());

        if (asset_pack_build(rv_paths_data(), rv_paths()->base) &&
            asset_pack_open_existing(rv_paths()->base)) {
            rvnx_log_print(4, "pack", "built, %zu entries", asset_pack_entry_count());
        } else {
            rvnx_log_print(5, "pack", "build failed (%s) -- reading files directly",
                           asset_pack_error());
        }
    }

    /* One line in the log that says exactly what content was found. When a
     * user reports a problem this is the first thing worth seeing, and the
     * cached path makes it free on every boot after the first. */
    {
        char summary[256];
        rv_assets_summarise(&assets, summary, sizeof summary);
        rvnx_log_print(4, "revoltnx", "%s", summary);

        rv_net_summarise(summary, sizeof summary);
        rvnx_log_print(4, "revoltnx", "%s", summary);
    }

    /* Before any module code runs: the canary register and the arena that
     * libc_shim's mmap carves from. */
    install_bionic_tls(g_main_tls);
    pps_mmap_arena_init();

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK |
                 SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER) != 0) {
        fatal_error("SDL_Init failed: %s", SDL_GetError());
    }

    /* Before the engine runs: RVGL's own gamecontrollerdb.txt has no
     * platform:Switch entries, so without this the pad enumerates as a raw
     * joystick and every button lands somewhere arbitrary. See rv_input.h. */
    startup_status_update("mapping controllers");
    rv_video_configure(rv_paths()->base);
    rv_input_configure_text(rv_paths()->base);
    rv_input_seed_settings(rv_paths_data());
    rv_input_install_mappings();
    rv_input_install_event_watch();
    rv_cursor_init(rv_paths_data());

    for (size_t i = 0; i < RV_MODULE_COUNT; ++i)
        LoadModule(&g_modules[i]);

    fn_sdl_main sdl_main = (fn_sdl_main)so_try_find_addr_rx(&RV_ENGINE->module, "SDL_main");
    if (sdl_main == NULL) {
        fatal_error("libmain.so exports no SDL_main.\n\n"
                    "This port expects RVGL's Android build.");
    }

    /* RVGL resolves its data files relative to the working directory. */
    if (chdir(rv_paths_data()) != 0)
        fatal_error("cannot enter %s", rv_paths_data());

    /* Releases the console; the engine owns the screen from here. */
    startup_status_end();

    /* MUST follow startup_status_end. stdout still refers to the console
     * device that consoleExit has just torn down, and the first printf after
     * this point -- libc_shim's fopen_fake does one -- would fault inside the
     * console renderer on a NULL framebuffer. */
    rvnx_redirect_stdio(rv_paths()->log);

    /* argv[0] is all RVGL needs; command-line options are read from its own
     * rvgl.ini inside the data directory. */
    char arg0[] = "rvgl";
    char *engine_argv[] = { arg0, NULL };

    const int result = sdl_main(1, engine_argv);

    /* Teardown order is the reverse of setup. The engine's atexit handlers
     * already ran inside SDL_main, so nothing here may call back into it. */
    {
        char traffic[192];
        rv_net_traffic(traffic, sizeof traffic);
        rvnx_log_print(4, "revoltnx", "%s", traffic);

        char stats[128];
        rv_ramcache_stats(stats, sizeof stats);
        rvnx_log_print(4, "revoltnx", "%s", stats);
    }

    opensles_shutdown();
    SDL_Quit();
    rv_net_exit();

    return result;
}
