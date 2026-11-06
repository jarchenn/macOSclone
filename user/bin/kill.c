/* kill [-SIGNAL | -s SIGNAL] pid...   (default SIGTERM; names or numbers) */
#include <signal.h>
#include <stdlib.h>
#include <strings.h>
#include "fileutil.h"

static const struct { const char *name; int sig; } names[] = {
    { "HUP", SIGHUP }, { "INT", SIGINT }, { "QUIT", SIGQUIT }, { "ILL", SIGILL }, { "ABRT", SIGABRT },
    { "FPE", SIGFPE }, { "KILL", SIGKILL }, { "SEGV", SIGSEGV }, { "PIPE", SIGPIPE }, { "ALRM", SIGALRM },
    { "TERM", SIGTERM }, { "USR1", SIGUSR1 }, { "USR2", SIGUSR2 }, { "CHLD", SIGCHLD }, { "CONT", SIGCONT },
    { "STOP", SIGSTOP }, { "TSTP", SIGTSTP }, { "WINCH", SIGWINCH },
};

static int parse(const char *s)
{
    if (*s >= '0' && *s <= '9')
        return atoi(s);
    if (!strncasecmp(s, "SIG", 3))
        s += 3;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strcasecmp(s, names[i].name))
            return names[i].sig;
    return -1;
}

int main(int argc, char **argv)
{
    int sig = SIGTERM, i = 1, r = 0;
    if (i < argc && !strcmp(argv[i], "-l")) {
        for (size_t k = 0; k < sizeof names / sizeof names[0]; k++)
            printf("%d %s\n", names[k].sig, names[k].name);
        return 0;
    }
    if (i + 1 < argc && !strcmp(argv[i], "-s")) {
        sig = parse(argv[i + 1]);
        i += 2;
    } else if (i < argc && argv[i][0] == '-') {
        sig = parse(argv[i] + 1);
        i++;
    }
    if (sig < 0 || i >= argc) {
        fprintf(stderr, "usage: kill [-SIGNAL] pid...\n");
        return 2;
    }
    for (; i < argc; i++)
        if (kill((pid_t)atoi(argv[i]), sig) < 0)
            r |= fail("kill", argv[i]);
    return r;
}
