/* init: process 1. It gives itself the console (/dev/console, the kernel
 * log) as standard input/output, starts the display server and restarts it
 * when it exits (0.7: the desktop is a user process), and waits for
 * orphaned processes the kernel re-parents to it so they do not stay
 * zombies. It sleeps in sigsuspend until SIGCHLD arrives. */
#include <aurora.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define DISPLAY "/system/bin/display"
#define RESTARTS_PER_MINUTE 5

static void on_child(int s) { (void)s; }

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static pid_t start_display(void)
{
    char *argv[] = { "display", NULL };
    pid_t pid;
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    sigset_t none;
    sigemptyset(&none);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK);
    int r = posix_spawn(&pid, DISPLAY, NULL, &at, argv, environ);
    posix_spawnattr_destroy(&at);
    if (r != 0) {
        fprintf(stderr, "init: cannot start %s: %s\n", DISPLAY, strerror(r));
        return -1;
    }
    return pid;
}

int main(void)
{
    /* The kernel starts init on /dev/null; the console is the serial log. */
    int fd = open("/dev/console", O_RDWR);
    if (fd >= 0) {
        dup2(fd, 0);
        dup2(fd, 1);
        dup2(fd, 2);
        if (fd > 2)
            close(fd);
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
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

    uint64_t starts[RESTARTS_PER_MINUTE] = { 0 };
    int nstarts = 0;
    pid_t display = start_display();
    if (display > 0)
        starts[nstarts++ % RESTARTS_PER_MINUTE] = now_ms();
    for (;;) {
        int st;
        pid_t pid;
        while ((pid = waitpid(-1, &st, WNOHANG)) > 0) {
            if (pid != display)
                continue;
            display = -1;
            if (WIFSIGNALED(st))
                printf("init: display server killed by signal %d; restarting\n", WTERMSIG(st));
            else
                printf("init: display server exited with status %d; restarting\n", WEXITSTATUS(st));
            aurora_test_event("DISPLAY_EXIT status=%d", st);
            /* Give up after RESTARTS_PER_MINUTE starts within a minute. */
            uint64_t oldest = starts[nstarts % RESTARTS_PER_MINUTE];
            if (nstarts >= RESTARTS_PER_MINUTE && now_ms() - oldest < 60000) {
                printf("init: display server keeps failing; not restarting it\n");
                continue;
            }
            display = start_display();
            if (display > 0)
                starts[nstarts++ % RESTARTS_PER_MINUTE] = now_ms();
        }
        sigsuspend(&none); /* returns after the SIGCHLD handler ran */
    }
}
