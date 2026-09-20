/* rv_glue.c -- small helpers the reused files expect. MIT licensed. */

#include <stdarg.h>
#include <stdio.h>

#include "util.h"

void rvnx_log_write(const char *text);

/* opensles.c and the copied utility files log through this. */
int debugLogNote(const char *text, ...) {
    char buffer[512];
    va_list args;
    va_start(args, text);
    const int n = vsnprintf(buffer, sizeof buffer, text, args);
    va_end(args);
    rvnx_log_write(buffer);
    return n;
}

/* asset_pack.c reports its build progress and RAM-cache decisions through
 * this. The first-run pack build takes minutes, so those lines matter. */
int debugPrintf(const char *text, ...) {
    char buffer[512];
    va_list args;
    va_start(args, text);
    const int n = vsnprintf(buffer, sizeof buffer, text, args);
    va_end(args);
    rvnx_log_write(buffer);
    return n;
}

/* nx_pointer.c logs through this name. */
int log_printf(const char *text, ...) {
    char buffer[512];
    va_list args;
    va_start(args, text);
    const int n = vsnprintf(buffer, sizeof buffer, text, args);
    va_end(args);
    rvnx_log_write(buffer);
    return n;
}
