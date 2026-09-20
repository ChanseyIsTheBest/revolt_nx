/* pps_jni.h -- minimal.
 *
 * RVGL needs no JNI bridge. Verified against the binary: libmain.so has no
 * Java_* export, no JNI_OnLoad, and imports no JNIEnv function. The supplied
 * classes.dex is the SDL2 activity wrapper, which switch-sdl2 replaces
 * wholesale. Only the quit flag survives, because imports_helpers.c sets it
 * from its exit path.
 *
 * MIT licensed.
 */
#ifndef RVNX_PPS_JNI_H
#define RVNX_PPS_JNI_H

extern volatile int jni_quit_requested;

#endif
