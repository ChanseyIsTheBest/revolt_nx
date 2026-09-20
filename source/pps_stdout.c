/* pps_stdout.c -- see pps_stdout.h. MIT licensed.
 *
 * Without nxlink attached there is nowhere for printf to go, and a port that
 * fails silently is a port you cannot debug on retail hardware. Everything is
 * mirrored to a file on the SD card; the console is used only while it exists,
 * because writing to it after consoleExit faults. */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pps_stdout.h"

static FILE *g_log = NULL;
static int   g_to_console = 1;

/* The log is written from more threads than it looks.
 *
 * rvnx_log_write goes through one unbuffered FILE*, and its callers are not
 * all on the main thread: the SDL event watcher fires on whichever thread
 * pushed the event, RVGL decodes audio and writes screenshots on its own
 * threads, and libc_shim reports failed opens from wherever they happen.
 * Unbuffered fputs from several threads interleaves mid-line at best, and on
 * newlib the FILE's internal state is not protected at all. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

void rvnx_stdout_init(const char *log_path) {
    if (g_log != NULL || log_path == NULL) return;
    g_log = fopen(log_path, "w");
    /* Unbuffered: a crash must not lose the lines that explain it. */
    if (g_log != NULL) setvbuf(g_log, NULL, _IONBF, 0);
}

void pps_stdout_to_console(void) { g_to_console = 1; }
void pps_stdout_to_log(void)     { g_to_console = 0; }

/* Called before anything that might not come back -- a fatal error, or the
 * handover to the engine. The file is unbuffered, so this is belt and braces
 * against a crash losing the lines that explain it. */
/* After consoleExit, stdout still points at the console device that libnx has
 * just torn down. Anything that printf()s from then on walks into
 * ConsoleSwRenderer_drawChar with a NULL framebuffer and takes a data abort at
 * address 0 -- which is exactly how the first run that reached SDL_main died,
 * from a printf inside libc_shim's fopen_fake.
 *
 * Pointing stdout and stderr at the log file fixes the whole class rather than
 * the one call site: any of the reused files may print, and RVGL's own printf
 * import is separately routed. If the reopen fails the stream is left closed,
 * which still beats the console -- newlib returns EOF rather than faulting. */
void rvnx_redirect_stdio(const char *log_path) {
    if (log_path == NULL) return;

    if (freopen(log_path, "a", stdout) != NULL)
        setvbuf(stdout, NULL, _IOLBF, 0);
    if (freopen(log_path, "a", stderr) != NULL)
        setvbuf(stderr, NULL, _IONBF, 0);

    /* rvnx_log_write would otherwise write every line twice, once to its own
     * handle and once to stdout, which is now the same file. */
    g_to_console = 0;
}

void rvnx_log_flush(void) {
    pthread_mutex_lock(&g_lock);
    if (g_log != NULL) fflush(g_log);
    fflush(stdout);
    pthread_mutex_unlock(&g_lock);
}

/* libc_shim routes the module's stdout here. Keeping it in one place means a
 * single fflush covers everything the engine printed. */
void rvnx_log_write(const char *text) {
    if (text == NULL) return;

    pthread_mutex_lock(&g_lock);
    if (g_log != NULL) fputs(text, g_log);
    if (g_to_console) fputs(text, stdout);
    pthread_mutex_unlock(&g_lock);
}
