/* init: process 1. The kernel re-parents orphaned processes to it; init
 * waits for them so they do not stay zombies. It sleeps in sigsuspend
 * until SIGCHLD arrives. */
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void on_child(int s) { (void)s; }

int main(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_child;
    sigaction(SIGCHLD, &sa, NULL);
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    sigset_t block, none;
    sigemptyset(&block);
    sigaddset(&block, SIGCHLD);
    sigprocmask(SIG_BLOCK, &block, &none);
    sigdelset(&none, SIGCHLD);
    for (;;) {
        int st;
        while (waitpid(-1, &st, WNOHANG) > 0)
            ;
        sigsuspend(&none); /* returns after the SIGCHLD handler ran */
    }
}
