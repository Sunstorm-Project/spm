#include "process.h"
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

int spm_run(char *const arguments[], int output_fd)
{
    pid_t child, waited;
    int status;
    if (!arguments || !arguments[0] || arguments[0][0] != '/') return -1;
    child = fork();
    if (child < 0) return -1;
    if (child == 0) {
        if (output_fd >= 0 && output_fd != STDOUT_FILENO) {
            if (dup2(output_fd, STDOUT_FILENO) < 0) _exit(126);
            close(output_fd);
        }
        execv(arguments[0], arguments);
        _exit(127);
    }
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited < 0 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}
