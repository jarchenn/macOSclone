/* sh: a tiny user-space shell. Runs programs from /system/bin with spawn()
 * and wait(); everything happens through system calls. */
#include "aurora.h"

int main(void)
{
    char line[200];
    printf("Aurora user shell (pid %d). Run any program in /system/bin; exit to quit.\n", getpid());
    for (;;) {
        printf("ush> ");
        if (readline(line, sizeof line) < 0)
            break;
        char *cmd = line;
        while (*cmd == ' ')
            cmd++;
        if (!*cmd)
            continue;
        if (!strcmp(cmd, "exit"))
            break;
        char *args = strchr(cmd, ' ');
        if (args)
            *args++ = '\0';
        char path[96];
        snprintf(path, sizeof path, "/system/bin/%s", cmd);
        int pid = spawn(path, args ? args : "");
        if (pid < 0) {
            printf("ush: %s: %s\n", cmd, strerror(pid));
            continue;
        }
        int status = wait(pid);
        printf("ush: %s exited with status %d\n", cmd, status);
    }
    printf("ush: bye\n");
    return 0;
}
