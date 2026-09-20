/* pps_jni.c -- see pps_jni.h. MIT licensed. */
#include "pps_jni.h"

/* Set by imports_helpers.c's exit path; main.c's loop watches it so the engine
 * can ask to quit without calling exit() out from under libnx. */
volatile int jni_quit_requested = 0;
