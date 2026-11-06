/* proctest: process-model checks (0.6 acceptance).
 *  - a child that segfaults is reported to its parent: WIFSIGNALED, SIGSEGV
 *  - exit status passes through waitpid; WNOHANG; ECHILD
 *  - execve replaces the image and keeps the pid; close-on-exec descriptors
 *    are closed in the new image, others survive
 *  - posix_spawn with file actions (dup2 into stdout)
 *  - an orphan is re-parented to init (pid 1)
 *  - pipes: EOF after the last writer closes, EPIPE with SIGPIPE ignored,
 *    death by SIGPIPE by default, O_NONBLOCK gives EAGAIN, poll()
 *  - dup/dup2/fcntl */
#define _GNU_SOURCE /* pipe2 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

extern char **environ;
static int passed, total;

static void check(const char *what, int ok)
{
    total++;
    passed += ok != 0;
    printf("proctest: %s %s\n", ok ? "ok  " : "FAIL", what);
}

static void test_segv(void)
{
    pid_t p = fork();
    if (p == 0) {
        *(volatile int *)16 = 1;
        _exit(0);
    }
    int st = 0;
    pid_t r = waitpid(p, &st, 0);
    printf("proctest: child %d status %#x\n", (int)p, st);
    check("segfaulting child reported as WIFSIGNALED/SIGSEGV", r == p && WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV);
}

static void test_wait(void)
{
    pid_t p = fork();
    if (p == 0) {
        usleep(200000);
        _exit(42);
    }
    int st = 0;
    check("WNOHANG returns 0 while the child runs", waitpid(p, &st, WNOHANG) == 0);
    check("exit status 42 through waitpid", waitpid(p, &st, 0) == p && WIFEXITED(st) && WEXITSTATUS(st) == 42);
    errno = 0;
    check("waitpid without children: ECHILD", waitpid(-1, &st, 0) < 0 && errno == ECHILD);
}

static void test_exec(const char *self)
{
    int keep = open("/system/about.txt", O_RDONLY);
    int drop = open("/system/about.txt", O_RDONLY | O_CLOEXEC);
    pid_t p = fork();
    if (p == 0) {
        char a[16], b[16];
        snprintf(a, sizeof a, "%d", keep);
        snprintf(b, sizeof b, "%d", drop);
        char *argv[] = { (char *)self, "--exec-child", a, b, NULL };
        execve(self, argv, environ);
        _exit(99);
    }
    int st = 0;
    waitpid(p, &st, 0);
    check("execve: same pid, cloexec fd closed, other fd kept", WIFEXITED(st) && WEXITSTATUS(st) == 7);
    close(keep);
    close(drop);
    errno = 0;
    char *argv[] = { "nope", NULL };
    check("execve of a missing file: ENOENT", execve("/system/bin/nope", argv, environ) < 0 && errno == ENOENT);
}

static int exec_child(int argc, char **argv)
{
    if (argc < 4)
        return 1;
    int keep = atoi(argv[2]), drop = atoi(argv[3]);
    int ok = fcntl(keep, F_GETFD) >= 0 && fcntl(drop, F_GETFD) < 0 && errno == EBADF;
    return ok ? 7 : 1;
}

static void test_spawn(void)
{
    int fds[2];
    pipe(fds);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    char *argv[] = { "echo", "spawned", "ok", NULL };
    pid_t p;
    int r = posix_spawn(&p, "/system/bin/echo", &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    char buf[64] = { 0 };
    ssize_t n = read(fds[0], buf, sizeof buf - 1);
    close(fds[0]);
    int st = 0;
    if (r == 0)
        waitpid(p, &st, 0);
    check("posix_spawn with dup2 file action", r == 0 && n > 0 && !strcmp(buf, "spawned ok\n") && WIFEXITED(st));
}

static void test_orphan(void)
{
    int fds[2];
    pipe(fds);
    pid_t p = fork();
    if (p == 0) {
        pid_t q = fork();
        if (q == 0) {
            usleep(300000); /* outlive the parent */
            char msg[16];
            int n = snprintf(msg, sizeof msg, "%d", (int)getppid());
            write(fds[1], msg, (size_t)n);
            _exit(0);
        }
        _exit(0);
    }
    close(fds[1]);
    waitpid(p, NULL, 0);
    char buf[16] = { 0 };
    read(fds[0], buf, sizeof buf - 1);
    close(fds[0]);
    check("orphan re-parented to init (getppid() == 1)", atoi(buf) == 1);
}

static void test_pipes(void)
{
    int fds[2];
    pipe(fds);
    write(fds[1], "abc", 3);
    close(fds[1]);
    char b[8];
    ssize_t a = read(fds[0], b, sizeof b), e = read(fds[0], b, sizeof b);
    close(fds[0]);
    check("pipe: data, then EOF after the last writer closes", a == 3 && e == 0);

    pipe(fds);
    close(fds[0]);
    signal(SIGPIPE, SIG_IGN);
    errno = 0;
    check("write without readers: EPIPE (SIGPIPE ignored)", write(fds[1], "x", 1) < 0 && errno == EPIPE);
    signal(SIGPIPE, SIG_DFL);
    close(fds[1]);

    pipe(fds);
    pid_t p = fork();
    if (p == 0) {
        close(fds[0]);
        usleep(100000);
        write(fds[1], "x", 1);
        _exit(0);
    }
    close(fds[0]);
    close(fds[1]);
    int st = 0;
    waitpid(p, &st, 0);
    check("write without readers kills with SIGPIPE by default", WIFSIGNALED(st) && WTERMSIG(st) == SIGPIPE);

    pipe2(fds, O_NONBLOCK);
    errno = 0;
    check("O_NONBLOCK read on an empty pipe: EAGAIN", read(fds[0], b, 1) < 0 && errno == EAGAIN);
    struct pollfd pf[2] = { { fds[0], POLLIN, 0 }, { fds[1], POLLOUT, 0 } };
    int n1 = poll(pf, 2, 0);
    int r_empty = pf[0].revents, w_ok = pf[1].revents & POLLOUT;
    write(fds[1], "y", 1);
    int n2 = poll(pf, 1, 1000);
    check("poll: writable, then readable after a write", n1 == 1 && r_empty == 0 && w_ok && n2 == 1 &&
                                                            (pf[0].revents & POLLIN));
    /* A write of up to PIPE_BUF bytes is atomic: with 4095 bytes free a
     * 4096-byte non-blocking write fails whole. Filled byte by byte, the
     * pipe takes 4095 more bytes, then poll reports it not writable. */
    char big[4096];
    memset(big, 'z', sizeof big);
    errno = 0;
    int atomic = write(fds[1], big, sizeof big) < 0 && errno == EAGAIN;
    ssize_t filled = 0, w;
    while ((w = write(fds[1], big, 1)) > 0)
        filled += w;
    pf[1].revents = 0;
    poll(&pf[1], 1, 0);
    check("full pipe: atomic PIPE_BUF writes, EAGAIN, poll not writable",
          atomic && w < 0 && errno == EAGAIN && filled == 4095 && !(pf[1].revents & POLLOUT));
    close(fds[0]);
    close(fds[1]);
}

static void test_dup(void)
{
    int fd = open("/tmp/proctest.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
    int d = dup(fd);
    write(fd, "hello", 5);
    int ok = lseek(d, 0, SEEK_CUR) == 5; /* shared offset */
    int t = dup2(fd, 20);
    ok = ok && t == 20 && fcntl(20, F_GETFD) == 0;
    ok = ok && fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 && fcntl(fd, F_GETFD) == FD_CLOEXEC;
    int fl = fcntl(fd, F_GETFL);
    ok = ok && (fl & O_ACCMODE) == O_RDWR;
    int hi = fcntl(fd, F_DUPFD, 30);
    ok = ok && hi >= 30;
    close(fd);
    close(d);
    close(t);
    close(hi);
    unlink("/tmp/proctest.txt");
    check("dup/dup2/fcntl (shared offset, FD_CLOEXEC, F_DUPFD)", ok);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--exec-child"))
        return exec_child(argc, argv);
    setvbuf(stdout, NULL, _IOLBF, 0);
    test_segv();
    test_wait();
    test_exec("/system/bin/proctest");
    test_spawn();
    test_orphan();
    test_pipes();
    test_dup();
    printf("proctest: %d/%d passed\n", passed, total);
    return passed == total ? 0 : 1;
}
