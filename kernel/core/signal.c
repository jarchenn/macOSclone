/* POSIX-style signals (0.6): generation, delivery on the way back to user
 * mode, the signal frame and sigreturn, and CPU exceptions as signals.
 *
 * Pending sets are updated with atomic operations because the timer
 * (SIGALRM) sets bits without the BKL; everything else runs under it. */
#include "core/process.h"
#include "core/sched.h"
#include "core/spinlock.h"
#include "core/log.h"
#include "core/time.h"
#include "arch/cpu.h"
#include "arch/x86_64/io.h"
#include "lib.h"

#define BIT(sig) (1ull << ((sig) - 1))
#define UNBLOCKABLE (BIT(AURORA_SIGKILL) | BIT(AURORA_SIGSTOP))

static const char *const sig_names[AURORA_NSIG] = {
    [AURORA_SIGHUP] = "SIGHUP", [AURORA_SIGINT] = "SIGINT", [AURORA_SIGQUIT] = "SIGQUIT",
    [AURORA_SIGILL] = "SIGILL", [AURORA_SIGTRAP] = "SIGTRAP", [AURORA_SIGABRT] = "SIGABRT",
    [AURORA_SIGFPE] = "SIGFPE", [AURORA_SIGKILL] = "SIGKILL", [AURORA_SIGBUS] = "SIGBUS",
    [AURORA_SIGSEGV] = "SIGSEGV", [AURORA_SIGSYS] = "SIGSYS", [AURORA_SIGPIPE] = "SIGPIPE",
    [AURORA_SIGALRM] = "SIGALRM", [AURORA_SIGTERM] = "SIGTERM", [AURORA_SIGURG] = "SIGURG",
    [AURORA_SIGSTOP] = "SIGSTOP", [AURORA_SIGTSTP] = "SIGTSTP", [AURORA_SIGCONT] = "SIGCONT",
    [AURORA_SIGCHLD] = "SIGCHLD", [AURORA_SIGTTIN] = "SIGTTIN", [AURORA_SIGTTOU] = "SIGTTOU",
    [AURORA_SIGWINCH] = "SIGWINCH", [AURORA_SIGUSR1] = "SIGUSR1", [AURORA_SIGUSR2] = "SIGUSR2",
};

/* Default action "ignore". Job-control stops are not implemented (0.6
 * defers job control), so SIGSTOP/SIGTSTP/SIGTTIN/SIGTTOU are ignored by
 * default too; SIGKILL always terminates. */
static bool default_ignores(int sig)
{
    return sig == AURORA_SIGCHLD || sig == AURORA_SIGURG || sig == AURORA_SIGWINCH || sig == AURORA_SIGCONT ||
           sig == AURORA_SIGSTOP || sig == AURORA_SIGTSTP || sig == AURORA_SIGTTIN || sig == AURORA_SIGTTOU;
}

static bool ignored(const struct process *p, int sig)
{
    if (sig == AURORA_SIGKILL)
        return false;
    uint64_t h = p->sig[sig].handler;
    return h == AURORA_SIG_IGN || (h == AURORA_SIG_DFL && default_ignores(sig));
}

static void wake_for_signal(struct task *t)
{
    sched_wake(t);
    sched_kick(t);
}

int signal_send(struct process *p, int sig)
{
    if (!p || p->state != PROC_RUNNING)
        return AE_SRCH;
    if (sig == 0)
        return 0;
    if (sig < 1 || sig >= AURORA_NSIG)
        return AE_INVAL;
    if (ignored(p, sig))
        return 0; /* discarded at generation */
    __atomic_or_fetch(&p->sigpending, BIT(sig), __ATOMIC_SEQ_CST);
    for (int i = 0; i < p->nthreads; i++) {
        struct task *t = p->threads[i];
        if (t && (sig == AURORA_SIGKILL || !(t->sigmask & BIT(sig)))) {
            wake_for_signal(t);
            if (sig != AURORA_SIGKILL)
                break;
        }
    }
    return 0;
}

int signal_send_thread(struct task *t, int sig)
{
    struct process *p = t ? t->proc : NULL;
    if (!p)
        return AE_SRCH;
    if (sig < 1 || sig >= AURORA_NSIG)
        return AE_INVAL;
    if (ignored(p, sig))
        return 0;
    __atomic_or_fetch(&t->sigpending, BIT(sig), __ATOMIC_SEQ_CST);
    wake_for_signal(t);
    return 0;
}

int signal_send_group(int pgid, int sig)
{
    int n = 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        struct process *p = process_at(i);
        if (p && p->state == PROC_RUNNING && p->pgid == pgid) {
            signal_send(p, sig);
            n++;
        }
    }
    return n ? 0 : AE_SRCH;
}

int process_kill(int pid) { return signal_send(process_get(pid), AURORA_SIGKILL); }

static uint64_t deliverable(const struct task *t, const struct process *p)
{
    uint64_t pend = (__atomic_load_n(&t->sigpending, __ATOMIC_SEQ_CST) |
                     __atomic_load_n(&p->sigpending, __ATOMIC_SEQ_CST));
    return pend & ~(t->sigmask & ~UNBLOCKABLE);
}

bool task_interrupted(struct task *t)
{
    struct process *p = t ? t->proc : NULL;
    if (!p)
        return false;
    if (p->exiting || t->exiting)
        return true;
    uint64_t d = deliverable(t, p);
    for (int sig = 1; d && sig < AURORA_NSIG; sig++)
        if ((d & BIT(sig)) && !ignored(p, sig))
            return true;
    return false;
}

bool process_kill_pending(const struct process *p)
{
    UNUSED(p);
    return task_interrupted(sched_current());
}

/* Takes the lowest deliverable signal off the pending sets (thread first). */
static int take_signal(struct task *t, struct process *p)
{
    uint64_t d = deliverable(t, p);
    if (!d)
        return 0;
    int sig = __builtin_ctzll(d) + 1;
    if (d & BIT(AURORA_SIGKILL))
        sig = AURORA_SIGKILL;
    if (__atomic_load_n(&t->sigpending, __ATOMIC_SEQ_CST) & BIT(sig))
        __atomic_and_fetch(&t->sigpending, ~BIT(sig), __ATOMIC_SEQ_CST);
    else
        __atomic_and_fetch(&p->sigpending, ~BIT(sig), __ATOMIC_SEQ_CST);
    return sig;
}

static bool restartable(uint32_t nr)
{
    return nr != SYS_NANOSLEEP && nr != SYS_SIGSUSPEND && nr != SYS_POLL && nr != SYS_FUTEX;
}

static void kill_by_signal(struct process *p, int sig)
{
    char reason[96];
    if (p->fault[0] && p->fault_sig == sig)
        str_copy(reason, p->fault, sizeof reason);
    else
        k_snprintf(reason, sizeof reason, "killed by %s", sig_names[sig] ? sig_names[sig] : "signal");
    proc_exit(sig, reason); /* wait status: the signal number */
}

/* Builds the frame and redirects f to the handler. */
static void deliver(struct interrupt_frame *f, struct task *t, struct process *p, int sig)
{
    struct sigdisp *d = &p->sig[sig];
    struct aurora_sigframe fr;
    memset(&fr, 0, sizeof fr);
    fr.sig = (uint64_t)sig;
    fr.r15 = f->r15; fr.r14 = f->r14; fr.r13 = f->r13; fr.r12 = f->r12;
    fr.r11 = f->r11; fr.r10 = f->r10; fr.r9 = f->r9; fr.r8 = f->r8;
    fr.rbp = f->rbp; fr.rdi = f->rdi; fr.rsi = f->rsi; fr.rdx = f->rdx;
    fr.rcx = f->rcx; fr.rbx = f->rbx; fr.rax = f->rax;
    fr.rip = f->rip;
    fr.rflags = f->rflags;
    fr.rsp = f->rsp;
    fr.mask = t->in_sigsuspend ? t->saved_mask : t->sigmask;
    fr.fsbase = t->fs_base;
    t->in_sigsuspend = false;
    /* An interrupted system call with SA_RESTART runs again after the
     * handler: back up over `int $0x80` and restore the call number. */
    if (t->sys_restart && (int64_t)f->rax == -AURORA_EINTR && (d->flags & AURORA_SA_RESTART) &&
        restartable(t->sys_restart - 1)) {
        fr.rax = t->sys_restart - 1;
        fr.rip -= 2;
    }
    fpu_save(fr.fxsave); /* the interrupted code's live registers */
    uint64_t sp = ALIGN_DOWN(f->rsp - 128 - sizeof fr, 64);
    struct process *self = p;
    if (!copy_to_user(self, sp, &fr, sizeof fr) && !copy_to_user(self, sp - 8, &d->restorer, 8)) {
        uint8_t clean[512] __attribute__((aligned(16)));
        fpu_init_state(clean);
        fpu_restore(clean); /* handlers start with a clean FPU state */
        f->rip = d->handler;
        f->rsp = sp - 8;
        f->rdi = (uint64_t)sig;
        f->rsi = 0;
        f->rdx = sp;
        f->rax = 0;
        f->rflags &= ~(1ull << 10 | 1ull << 8); /* DF and TF clear */
        t->sigmask |= d->mask | ((d->flags & AURORA_SA_NODEFER) ? 0 : BIT(sig));
        t->sigmask &= ~UNBLOCKABLE;
        if (d->flags & AURORA_SA_RESETHAND) {
            d->handler = AURORA_SIG_DFL;
            d->flags &= ~AURORA_SA_SIGINFO;
        }
        TEST_EVENT("SIG_DELIVER pid=%d tid=%d sig=%d", p->pid, t->tid, sig);
        return;
    }
    /* The stack cannot take the frame: the process dies of SIGSEGV. */
    str_copy(p->fault, "segmentation fault: no room for a signal frame", sizeof p->fault);
    p->fault_sig = AURORA_SIGSEGV;
    kill_by_signal(p, AURORA_SIGSEGV);
}

void signal_return_to_user(struct interrupt_frame *f)
{
    struct task *t = sched_current();
    struct process *p = t ? t->proc : NULL;
    if (!p)
        return;
    if (!p->exiting && !t->exiting && !deliverable(t, p) && !t->in_sigsuspend) {
        t->sys_restart = 0;
        return;
    }
    bkl_lock();
    for (;;) {
        if (p->exiting || t->exiting)
            proc_thread_exit(); /* never returns */
        int sig = take_signal(t, p);
        if (!sig)
            break;
        uint64_t h = p->sig[sig].handler;
        if (sig == AURORA_SIGKILL || h == AURORA_SIG_DFL) {
            if (sig != AURORA_SIGKILL && default_ignores(sig))
                continue;
            kill_by_signal(p, sig);
        }
        if (h == AURORA_SIG_IGN)
            continue;
        deliver(f, t, p, sig);
        break; /* one handler frame at a time */
    }
    if (t->in_sigsuspend) {
        t->sigmask = t->saved_mask;
        t->in_sigsuspend = false;
    }
    t->sys_restart = 0;
    bkl_unlock();
}

long signal_sigreturn(struct interrupt_frame *f)
{
    struct process *p = process_current();
    struct task *t = sched_current();
    struct aurora_sigframe fr;
    if (copy_from_user(p, &fr, f->rsp, sizeof fr)) {
        str_copy(p->fault, "segmentation fault: bad signal frame", sizeof p->fault);
        p->fault_sig = AURORA_SIGSEGV;
        kill_by_signal(p, AURORA_SIGSEGV);
    }
    f->r15 = fr.r15; f->r14 = fr.r14; f->r13 = fr.r13; f->r12 = fr.r12;
    f->r11 = fr.r11; f->r10 = fr.r10; f->r9 = fr.r9; f->r8 = fr.r8;
    f->rbp = fr.rbp; f->rdi = fr.rdi; f->rsi = fr.rsi; f->rdx = fr.rdx;
    f->rcx = fr.rcx; f->rbx = fr.rbx; f->rax = fr.rax;
    f->rip = fr.rip;
    f->rsp = fr.rsp;
    /* Only arithmetic flags, TF, DF and AC come from user memory. */
    f->rflags = (fr.rflags & 0x40DD5ull) | 0x202;
    f->cs = USER_CS;
    f->ss = USER_SS;
    t->sigmask = fr.mask & ~UNBLOCKABLE;
    t->fs_base = fr.fsbase;
    wrmsr(MSR_FS_BASE, fr.fsbase);
    /* MXCSR reserved bits make FXRSTOR fault: drop them. */
    uint32_t mxcsr, mask;
    memcpy(&mxcsr, fr.fxsave + 24, 4);
    memcpy(&mask, fr.fxsave + 28, 4);
    if (!mask)
        mask = 0xFFBF;
    mxcsr &= mask;
    memcpy(fr.fxsave + 24, &mxcsr, 4);
    fpu_restore(fr.fxsave);
    return 0;
}

void process_user_exception(struct interrupt_frame *f)
{
    struct task *t = sched_current();
    struct process *p = t ? t->proc : NULL;
    if (!p)
        return;
    uint64_t v = f->vector;
    char reason[96];
    int sig;
    if (v == 14) {
        uint64_t cr2 = read_cr2();
        cpu_sti();
        bkl_lock();
        int r = mm_fault(&p->mm, cr2, (f->error_code & 2) != 0, (f->error_code & 16) != 0);
        bkl_unlock();
        if (r == 0)
            return; /* demand page or copy-on-write: retry the instruction */
        k_snprintf(reason, sizeof reason, "segmentation fault: %s %s at %llx (rip %llx)",
                   (f->error_code & 2) ? "write" : ((f->error_code & 16) ? "exec" : "read"),
                   (f->error_code & 1) ? "denied" : "unmapped", (unsigned long long)cr2,
                   (unsigned long long)f->rip);
        sig = AURORA_SIGSEGV;
    } else {
        static const char *names[20] = { "divide error", "debug", "NMI", "breakpoint", "overflow",
            "bound range", "invalid opcode", "device not available", "double fault", "",
            "invalid TSS", "segment not present", "stack fault", "general protection fault", "page fault",
            "", "x87 error", "alignment check", "machine check", "SIMD error" };
        k_snprintf(reason, sizeof reason, "%s (rip %llx)", v < 20 && names[v][0] ? names[v] : "CPU exception",
                   (unsigned long long)f->rip);
        switch (v) {
        case 0: case 16: case 19: sig = AURORA_SIGFPE; break;
        case 6: sig = AURORA_SIGILL; break;
        case 1: case 3: sig = AURORA_SIGTRAP; break;
        case 17: sig = AURORA_SIGBUS; break;
        default: sig = AURORA_SIGSEGV; break;
        }
        cpu_sti();
    }
    bkl_lock();
    str_copy(p->fault, reason, sizeof p->fault);
    p->fault_sig = sig;
    /* A synchronous fault cannot be blocked or ignored: if it is, the
     * default action (termination) applies. */
    if ((t->sigmask & BIT(sig)) || p->sig[sig].handler == AURORA_SIG_IGN) {
        t->sigmask &= ~BIT(sig);
        p->sig[sig].handler = AURORA_SIG_DFL;
    }
    __atomic_or_fetch(&t->sigpending, BIT(sig), __ATOMIC_SEQ_CST);
    bkl_unlock();
}
