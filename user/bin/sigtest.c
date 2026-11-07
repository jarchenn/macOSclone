/* sigtest: signal delivery checks (0.6 acceptance).
 *  1. A computation held entirely in registers (16 general and 8 SSE
 *     registers) is interrupted by hundreds of SIGUSR1s sent by another
 *     process; the handler clobbers every register. After each sigreturn
 *     the computation must continue unchanged: its result is compared with
 *     a run that had the signal blocked.
 *  2. A blocked signal stays pending and is delivered when unblocked.
 *  3. A read blocked on a pipe returns EINTR when SIGALRM interrupts it,
 *     and is restarted transparently with SA_RESTART.
 *  4. A process killed by SIGTERM is reported through waitpid. */
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static volatile sig_atomic_t handled;
static int passed, total;

static void check(const char *what, int ok)
{
    total++;
    passed += ok != 0;
    printf("sigtest: %s %s\n", ok ? "ok  " : "FAIL", what);
}

/* Mixes values in rbx, rsi, rdi, r8-r15 and xmm0-xmm7 for `iters` rounds. */
static uint64_t compute(uint64_t iters)
{
    uint64_t out;
    __asm__ volatile(
        "mov $0x0123456789abcdef, %%rbx\n"
        "mov $3, %%rsi\n"
        "mov $5, %%rdi\n"
        "mov $7, %%r8\n"
        "mov $11, %%r9\n"
        "mov $13, %%r10\n"
        "mov $17, %%r11\n"
        "mov $19, %%r12\n"
        "mov $23, %%r13\n"
        "mov $29, %%r14\n"
        "mov $31, %%r15\n"
        "movq %%rbx, %%xmm0\n"
        "movq %%rsi, %%xmm1\n"
        "movq %%rdi, %%xmm2\n"
        "movq %%r8, %%xmm3\n"
        "movq %%r9, %%xmm4\n"
        "movq %%r10, %%xmm5\n"
        "movq %%r11, %%xmm6\n"
        "movq %%r12, %%xmm7\n"
        "1:\n"
        "add %%rbx, %%rsi\n"
        "rol $13, %%rsi\n"
        "xor %%rsi, %%rdi\n"
        "add %%rdi, %%r8\n"
        "rol $7, %%r8\n"
        "xor %%r8, %%r9\n"
        "add %%r9, %%r10\n"
        "rol $17, %%r10\n"
        "xor %%r10, %%r11\n"
        "add %%r11, %%r12\n"
        "xor %%r12, %%r13\n"
        "add %%r13, %%r14\n"
        "rol $3, %%r14\n"
        "xor %%r14, %%r15\n"
        "add %%r15, %%rbx\n"
        "paddq %%xmm0, %%xmm1\n"
        "pxor %%xmm1, %%xmm2\n"
        "paddq %%xmm2, %%xmm3\n"
        "pxor %%xmm3, %%xmm4\n"
        "paddq %%xmm4, %%xmm5\n"
        "pxor %%xmm5, %%xmm6\n"
        "paddq %%xmm6, %%xmm7\n"
        "pxor %%xmm7, %%xmm0\n"
        "dec %%rcx\n"
        "jnz 1b\n"
        "movq %%xmm0, %%rax\n"
        "xor %%rbx, %%rax\n"
        "xor %%rsi, %%rax\n"
        "xor %%rdi, %%rax\n"
        "xor %%r8, %%rax\n"
        "xor %%r9, %%rax\n"
        "xor %%r10, %%rax\n"
        "xor %%r11, %%rax\n"
        "xor %%r12, %%rax\n"
        "xor %%r13, %%rax\n"
        "xor %%r14, %%rax\n"
        "xor %%r15, %%rax\n"
        "movq %%xmm7, %%rdx\n"
        "xor %%rdx, %%rax\n"
        : "=a"(out), "+c"(iters)
        :
        : "rbx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "xmm0", "xmm1",
          "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "cc", "memory");
    return out;
}

static void clobber_handler(int sig)
{
    (void)sig;
    handled++;
    __asm__ volatile(
        "mov $-1, %%rax\n mov $-1, %%rbx\n mov $-1, %%rcx\n mov $-1, %%rdx\n"
        "mov $-1, %%rsi\n mov $-1, %%rdi\n mov $-1, %%r8\n mov $-1, %%r9\n"
        "mov $-1, %%r10\n mov $-1, %%r11\n mov $-1, %%r12\n mov $-1, %%r13\n"
        "mov $-1, %%r14\n mov $-1, %%r15\n"
        "pcmpeqd %%xmm0, %%xmm0\n pcmpeqd %%xmm1, %%xmm1\n pcmpeqd %%xmm2, %%xmm2\n pcmpeqd %%xmm3, %%xmm3\n"
        "pcmpeqd %%xmm4, %%xmm4\n pcmpeqd %%xmm5, %%xmm5\n pcmpeqd %%xmm6, %%xmm6\n pcmpeqd %%xmm7, %%xmm7\n"
        "pcmpeqd %%xmm8, %%xmm8\n pcmpeqd %%xmm15, %%xmm15\n"
        "xor %%eax, %%eax\n cmp %%rax, %%rbx\n" /* also change the flags */
        :
        :
        : "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "xmm0",
          "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm15", "cc", "memory");
    char tmp[32];
    snprintf(tmp, sizeof tmp, "%f", 1.5 * (double)handled); /* FPU work inside the handler */
}

static void test_registers(void)
{
    const uint64_t iters = 200000000;
    sigset_t usr1, old;
    sigemptyset(&usr1);
    sigaddset(&usr1, SIGUSR1);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = clobber_handler;
    sigaction(SIGUSR1, &sa, NULL);
    sigprocmask(SIG_BLOCK, &usr1, &old);
    uint64_t want = compute(iters);
    int fds[2];
    pipe(fds);
    pid_t me = getpid();
    pid_t sender = fork();
    if (sender == 0) {
        close(fds[1]);
        char c;
        read(fds[0], &c, 1); /* start signal */
        for (int i = 0; i < 2000; i++) {
            if (kill(me, SIGUSR1) < 0)
                _exit(0);
            usleep(1000);
        }
        _exit(0);
    }
    close(fds[0]);
    sigprocmask(SIG_SETMASK, &old, NULL);
    handled = 0;
    write(fds[1], "g", 1);
    uint64_t got = compute(iters);
    int n = handled;
    kill(sender, SIGKILL);
    waitpid(sender, NULL, 0);
    close(fds[1]);
    printf("sigtest: %d signals during the computation\n", n);
    printf("sigtest: want %016llx\n", (unsigned long long)want);
    printf("sigtest: got  %016llx\n", (unsigned long long)got);
    check("registers survive handlers that clobber them", n >= 20 && got == want);
}

static void count_handler(int s)
{
    (void)s;
    handled++;
}

static void test_pending(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = count_handler;
    sigaction(SIGUSR2, &sa, NULL);
    sigset_t s, old, pend;
    sigemptyset(&s);
    sigaddset(&s, SIGUSR2);
    sigprocmask(SIG_BLOCK, &s, &old);
    handled = 0;
    kill(getpid(), SIGUSR2);
    sigpending(&pend);
    int was_pending = sigismember(&pend, SIGUSR2) && handled == 0;
    printf("sigtest: pending set %#lx, handled before unblock %d\n", (unsigned long)pend, (int)handled);
    sigprocmask(SIG_SETMASK, &old, NULL);
    printf("sigtest: handled after unblock %d\n", (int)handled);
    check("blocked signal stays pending, runs when unblocked", was_pending && handled == 1);
}

static void test_eintr(void)
{
    int fds[2];
    pipe(fds);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = count_handler;
    sigaction(SIGALRM, &sa, NULL); /* no SA_RESTART */
    handled = 0;
    alarm(1);
    char buf[8];
    errno = 0;
    ssize_t r = read(fds[0], buf, sizeof buf);
    check("blocking read interrupted: EINTR", r < 0 && errno == EINTR && handled == 1);

    sa.sa_flags = SA_RESTART;
    sigaction(SIGALRM, &sa, NULL);
    handled = 0;
    pid_t w = fork();
    if (w == 0) {
        usleep(1800000);
        write(fds[1], "ok", 2);
        _exit(0);
    }
    alarm(1);
    r = read(fds[0], buf, sizeof buf);
    waitpid(w, NULL, 0);
    check("SA_RESTART restarts the read after the handler", r == 2 && !memcmp(buf, "ok", 2) && handled == 1);
    close(fds[0]);
    close(fds[1]);
}

static void test_kill_status(void)
{
    pid_t p = fork();
    if (p == 0) {
        for (;;)
            pause();
    }
    usleep(100000);
    kill(p, SIGTERM);
    int st = 0;
    waitpid(p, &st, 0);
    check("SIGTERM death reported by waitpid", WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    test_registers();
    test_pending();
    test_eintr();
    test_kill_status();
    printf("sigtest: %d/%d passed\n", passed, total);
    return passed == total ? 0 : 1;
}
