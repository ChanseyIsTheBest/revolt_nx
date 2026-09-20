/* pps_diag.h -- lock-contention diagnostics, compiled out by default. MIT. */
#ifndef RVNX_PPS_DIAG_H
#define RVNX_PPS_DIAG_H

enum { DIAG_W_MUTEX = 0, DIAG_W_COND, DIAG_W_JOIN, DIAG_W_SEM };

#define diag_wait_enter(kind, object) ((void)(kind), (void)(object))
#define diag_wait_exit()              ((void)0)
#define diag_thread_register(e, i)    ((void)(e), (void)(i))
#define diag_thread_unregister()      ((void)0)

#endif
