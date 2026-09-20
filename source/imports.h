/* imports.h -- the resolver table. MIT licensed.
 *
 * libc_shim.c is compiled in unmodified and includes this by name for
 * dynlib_find_export, which backs its dlsym() shim. RVGL reaches dlsym through
 * libopenal.so, which dlopen()s libOpenSLES.so and resolves slCreateEngine and
 * five SL_IID_* ids by name -- none of them appears in any import table, so
 * without this lookup audio initialises, finds nothing and plays silence with
 * no error anywhere.
 */
#ifndef RVNX_IMPORTS_H
#define RVNX_IMPORTS_H

#include <stddef.h>
#include <stdint.h>
#include "so_util.h"

extern DynLibFunction dynlib_functions[];
extern const size_t   dynlib_numfunctions;

uintptr_t dynlib_find_export(const char *name);

#endif
