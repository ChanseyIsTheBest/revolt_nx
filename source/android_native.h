/* android_native.h -- forwarder.
 *
 * The reference ports impersonate android_native_app_glue. RVGL does not use
 * it: libmain.so imports no ANativeActivity, ALooper or AInputQueue symbol and
 * exports SDL_main, so SDL2 owns the loop. Nothing to declare.
 * MIT licensed.
 */
#ifndef RVNX_ANDROID_NATIVE_H
#define RVNX_ANDROID_NATIVE_H
#endif
