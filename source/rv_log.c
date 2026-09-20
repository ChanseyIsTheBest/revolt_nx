/* rv_log.c -- see rv_log.h. MIT licensed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "rv_log.h"
#include "pps_jni.h"
#include "pps_stdout.h"   /* rvnx_log_flush */


static const char *PriorityTag(int prio) {
    switch (prio) {
        case 2: return "V"; case 3: return "D"; case 4: return "I";
        case 5: return "W"; case 6: return "E"; case 7: return "F";
        default: return "?";
    }
}

int rvnx_log_vprint(int prio, const char *tag, const char *fmt, va_list args) {
    /* Android priorities: 2 VERBOSE, 3 DEBUG, 4 INFO, 5 WARN, 6 ERROR.
     *
     * The modules are chatty at DEBUG -- OpenAL alone prints about sixty
     * "GetConfigValue: Key ... not found" lines every boot -- and on an SD
     * card that is slow as well as noisy. Warnings and errors always get
     * through; build with RVNX_DEBUG_LOG=1 to see the rest. */
#if !defined(RVNX_DEBUG_LOG) || !RVNX_DEBUG_LOG
    if (prio < 4) return 0;
#endif

    char body[1024];
    vsnprintf(body, sizeof body, fmt ? fmt : "", args);

    char line[1152];
    const int n = snprintf(line, sizeof line, "[%s/%s] %s\n",
                           PriorityTag(prio), tag ? tag : "rvgl", body);
    rvnx_log_write(line);
    return n;
}

int rvnx_log_print(int prio, const char *tag, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const int n = rvnx_log_vprint(prio, tag, fmt, args);
    va_end(args);
    return n;
}

int rvnx_log_write_prio(int prio, const char *tag, const char *text) {
    return rvnx_log_print(prio, tag, "%s", text ? text : "");
}

void rvnx_log_assert(const char *cond, const char *tag, const char *fmt, ...) {
    if (fmt != NULL) {
        va_list args;
        va_start(args, fmt);
        rvnx_log_vprint(7, tag, fmt, args);
        va_end(args);
    } else {
        rvnx_log_print(7, tag, "assertion failed: %s", cond ? cond : "?");
    }
    abort();
}

void log_write(char level, const char *fmt, ...) {
    int priority;
    switch (level) {
        case 'E': priority = 6; break;
        case 'W': priority = 5; break;
        default:  priority = 4; break;
    }

    va_list args;
    va_start(args, fmt);
    rvnx_log_vprint(priority, "rvgl", fmt, args);
    va_end(args);
}

void log_flush(void) { rvnx_log_flush(); }
void log_close(void) { rvnx_log_flush(); }

void rvnx_noop(void) {}

void rvnx_exit(int code) {
    rvnx_log_print(4, "revoltnx", "engine called exit(%d)", code);
    jni_quit_requested = 1;
    /* The engine expects exit() never to return. Nothing in the teardown path
     * calls back into module code, so handing control to newlib is safe here
     * -- the flag above is what lets main.c skip its own cleanup. */
    exit(code);
}
