#ifndef SPM_PROCESS_H
#define SPM_PROCESS_H
/* Execute an absolute program path with literal arguments; no shell.
 * output_fd >= 0 redirects stdout (not stderr). Returns exit status or -1. */
int spm_run(char *const arguments[], int output_fd);
#endif
