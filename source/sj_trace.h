/* sj_trace.h -- forwarder; sj_trace_open is declared in compat_stubs.h.
 *
 * Deliberately not a macro. A macro named like a function collides with the
 * declaration, and something may yet want its address. It compiles to nothing
 * unless RVNX_TRACE_IO is set. MIT licensed. */
#ifndef RVNX_SJ_TRACE_H
#define RVNX_SJ_TRACE_H
#include "compat_stubs.h"
#endif
