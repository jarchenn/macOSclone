/* Signals: sigaction and friends on top of the kernel's dispositions. Every
 * handler returns through __aurora_sigreturn (crt0.S). */
#include "sys.h"
#include <signal.h>
#include <string.h>
#include <unistd.h>

extern void __aurora_sigreturn(void);

int sigaction(int sig, const struct sigaction *act, struct sigaction *old)
{
    struct aurora_sigaction a, o;
    struct aurora_sigaction *ap = NULL;
    if (act) {
        memset(&a, 0, sizeof a);
        a.handler = (uint64_t)(uintptr_t)act->sa_handler;
        a.restorer = (uint64_t)(uintptr_t)__aurora_sigreturn;
        a.mask = (uint64_t)act->sa_mask;
        a.flags = (uint32_t)act->sa_flags;
        ap = &a;
    }
    long r = __sys3(SYS_SIGACTION, sig, ap, old ? &o : NULL);
    if (r < 0)
        return (int)__ret(r);
    if (old) {
        memset(old, 0, sizeof *old);
        old->sa_handler = (_sig_func_ptr)(uintptr_t)o.handler;
        old->sa_mask = (sigset_t)o.mask;
        old->sa_flags = (int)o.flags;
    }
    return 0;
}

_sig_func_ptr signal(int sig, _sig_func_ptr handler)
{
    struct sigaction a, o;
    memset(&a, 0, sizeof a);
    a.sa_handler = handler;
    a.sa_flags = SA_RESTART; /* BSD semantics, like most C libraries */
    if (sigaction(sig, &a, &o) < 0)
        return SIG_ERR;
    return o.sa_handler;
}

int sigprocmask(int how, const sigset_t *set, sigset_t *old)
{
    uint64_t s = set ? (uint64_t)*set : 0, o = 0;
    long r = __sys3(SYS_SIGPROCMASK, how, set ? &s : NULL, old ? &o : NULL);
    if (r < 0)
        return (int)__ret(r);
    if (old)
        *old = (sigset_t)o;
    return 0;
}

int pthread_sigmask(int how, const sigset_t *set, sigset_t *old)
{
    return sigprocmask(how, set, old) < 0 ? errno : 0; /* masks are per thread */
}

int sigpending(sigset_t *set)
{
    uint64_t s = 0;
    long r = __sys1(SYS_SIGPENDING, &s);
    if (r < 0)
        return (int)__ret(r);
    *set = (sigset_t)s;
    return 0;
}

int sigsuspend(const sigset_t *mask)
{
    uint64_t m = (uint64_t)*mask;
    return (int)__ret(__sys1(SYS_SIGSUSPEND, &m));
}

int pause(void)
{
    sigset_t cur;
    sigprocmask(SIG_BLOCK, NULL, &cur);
    return sigsuspend(&cur);
}

int raise(int sig) { return kill(getpid(), sig); }
int _raise_r(struct _reent *r, int sig)
{
    (void)r;
    return raise(sig);
}

int siginterrupt(int sig, int flag)
{
    struct sigaction a;
    if (sigaction(sig, NULL, &a) < 0)
        return -1;
    if (flag)
        a.sa_flags &= ~SA_RESTART;
    else
        a.sa_flags |= SA_RESTART;
    return sigaction(sig, &a, NULL);
}
