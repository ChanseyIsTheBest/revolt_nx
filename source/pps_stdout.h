/* pps_stdout.h -- routes stdout/stderr between the boot console and the log.
 * MIT licensed. */
#ifndef RVNX_PPS_STDOUT_H
#define RVNX_PPS_STDOUT_H

void rvnx_stdout_init(const char *log_path);
void rvnx_log_flush(void);
void rvnx_redirect_stdio(const char *log_path);
void pps_stdout_to_log(void);      /* after the console is torn down */
void pps_stdout_to_console(void);  /* before consoleInit, so errors show */

#endif
