/* POSIX threads on Aurora: kernel threads sharing the address space
 * (SYS_THREAD_CREATE), futex-based synchronisation, TLS through FS.
 *
 * Join uses the kernel's clear-tid futex: when a thread exits, the kernel
 * stores 0 into its descriptor's `tid` and wakes one waiter. Stacks and TLS
 * of finished threads are freed by whoever joins them (detached threads:
 * by the next pthread_create once the kernel reports them gone). */
#include "sys.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>

#define KEYS_MAX 64
#define DEFAULT_STACK (512 * 1024)

struct pthread {
    volatile uint32_t tid; /* cleared by the kernel at exit */
    int detached;
    void *(*fn)(void *);
    void *arg;
    void *ret;
    void *stack;
    size_t stack_size;
    void *tls;
    struct aurora_tcb *tcb;
    void *keys[KEYS_MAX];
    struct pthread *next_dead;
};

static struct pthread main_thread;
static struct pthread *dead_detached;
static pthread_mutex_t dead_lock = PTHREAD_MUTEX_INITIALIZER;

static long futex_wait(volatile uint32_t *addr, uint32_t val, int64_t timeout_ns)
{
    return __sys4(SYS_FUTEX, addr, AURORA_FUTEX_WAIT, val, timeout_ns);
}
static long futex_wake(volatile uint32_t *addr, int n) { return __sys3(SYS_FUTEX, addr, AURORA_FUTEX_WAKE, n); }

static struct pthread *self_desc(void) { return __tcb()->thread; }
static int self_tid(void) { return (int)self_desc()->tid; }

void __pthread_init_main(struct aurora_tcb *tcb)
{
    main_thread.tid = (uint32_t)__sys0(SYS_GETTID);
    main_thread.tcb = tcb;
    tcb->thread = &main_thread;
}

void __pthread_after_fork_child(void)
{
    /* The forking thread is the only thread of the child. */
    self_desc()->tid = (uint32_t)__sys0(SYS_GETTID);
}

pthread_t pthread_self(void) { return (pthread_t)self_desc(); }
int pthread_equal(pthread_t a, pthread_t b) { return a == b; }

static void free_thread(struct pthread *t)
{
    if (t->stack)
        munmap(t->stack, t->stack_size);
    free(t->tls);
    free(t);
}

static void reap_detached(void)
{
    pthread_mutex_lock(&dead_lock);
    struct pthread **pp = &dead_detached;
    while (*pp) {
        struct pthread *t = *pp;
        if (__atomic_load_n(&t->tid, __ATOMIC_ACQUIRE) == 0) {
            *pp = t->next_dead;
            free_thread(t);
        } else {
            pp = &t->next_dead;
        }
    }
    pthread_mutex_unlock(&dead_lock);
}

/* ---- keys ------------------------------------------------------------------ */

static struct {
    int used;
    void (*dtor)(void *);
} keys[KEYS_MAX];
static pthread_mutex_t keys_lock = PTHREAD_MUTEX_INITIALIZER;

int pthread_key_create(pthread_key_t *key, void (*dtor)(void *))
{
    pthread_mutex_lock(&keys_lock);
    for (int i = 0; i < KEYS_MAX; i++) {
        if (!keys[i].used) {
            keys[i].used = 1;
            keys[i].dtor = dtor;
            *key = (pthread_key_t)i;
            pthread_mutex_unlock(&keys_lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&keys_lock);
    return EAGAIN;
}

int pthread_key_delete(pthread_key_t key)
{
    if (key >= KEYS_MAX)
        return EINVAL;
    keys[key].used = 0;
    keys[key].dtor = NULL;
    return 0;
}

void *pthread_getspecific(pthread_key_t key) { return key < KEYS_MAX ? self_desc()->keys[key] : NULL; }

int pthread_setspecific(pthread_key_t key, const void *v)
{
    if (key >= KEYS_MAX || !keys[key].used)
        return EINVAL;
    self_desc()->keys[key] = (void *)v;
    return 0;
}

static void run_key_dtors(struct pthread *t)
{
    for (int round = 0; round < 4; round++) {
        int any = 0;
        for (int i = 0; i < KEYS_MAX; i++) {
            void *v = t->keys[i];
            if (v && keys[i].used && keys[i].dtor) {
                t->keys[i] = NULL;
                keys[i].dtor(v);
                any = 1;
            }
        }
        if (!any)
            break;
    }
}

/* ---- creation and exit ------------------------------------------------------- */

void __aurora_thread_main(struct pthread *t);
void __aurora_thread_main(struct pthread *t)
{
    void *r = t->fn(t->arg);
    pthread_exit(r);
}

extern void __aurora_thread_entry(void);

int pthread_create(pthread_t *out, const pthread_attr_t *attr, void *(*fn)(void *), void *arg)
{
    reap_detached();
    struct pthread *t = calloc(1, sizeof *t);
    if (!t)
        return EAGAIN;
    size_t ss = attr && attr->stacksize ? (size_t)attr->stacksize : DEFAULT_STACK;
    ss = (ss + 4095) & ~(size_t)4095;
    t->stack_size = ss + 4096; /* plus an inaccessible guard page */
    t->stack = mmap(NULL, t->stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (t->stack == MAP_FAILED) {
        free(t);
        return EAGAIN;
    }
    mprotect(t->stack, 4096, PROT_NONE);
    t->tls = malloc(__tls_area_size());
    if (!t->tls) {
        munmap(t->stack, t->stack_size);
        free(t);
        return EAGAIN;
    }
    t->tcb = __tls_init_area(t->tls);
    t->tcb->thread = t;
    t->fn = fn;
    t->arg = arg;
    t->detached = attr && attr->detachstate == PTHREAD_CREATE_DETACHED;
    uint64_t top = ((uint64_t)(uintptr_t)t->stack + t->stack_size) & ~15ull;
    struct aurora_thread ta = {
        .entry = (uint64_t)(uintptr_t)__aurora_thread_entry,
        .stack = top - 8,
        .arg = (uint64_t)(uintptr_t)t,
        .fsbase = (uint64_t)(uintptr_t)t->tcb,
        .clear_tid = (uint64_t)(uintptr_t)&t->tid,
    };
    /* tid is set before the thread can run: the kernel returns it, but the
     * thread may exit first, so publish a non-zero value in advance. */
    t->tid = 0xffffffffu;
    long r = __sys1(SYS_THREAD_CREATE, &ta);
    if (r < 0) {
        free_thread(t);
        return (int)-r == ENOMEM ? EAGAIN : (int)-r;
    }
    __atomic_compare_exchange_n(&t->tid, &(uint32_t){ 0xffffffffu }, (uint32_t)r, 0, __ATOMIC_ACQ_REL,
                                __ATOMIC_ACQUIRE);
    if (t->detached) {
        pthread_mutex_lock(&dead_lock);
        t->next_dead = dead_detached;
        dead_detached = t;
        pthread_mutex_unlock(&dead_lock);
    }
    *out = (pthread_t)t;
    return 0;
}

void pthread_exit(void *ret)
{
    struct pthread *t = self_desc();
    t->ret = ret;
    run_key_dtors(t);
    if (t == &main_thread) {
        /* The main thread leaving ends the process only when it is the
         * last thread; the kernel decides. */
        __sys1(SYS_THREAD_EXIT, 0);
    }
    __sys1(SYS_THREAD_EXIT, 0);
    for (;;)
        ;
}

int pthread_join(pthread_t th, void **ret)
{
    struct pthread *t = (struct pthread *)th;
    if (t == self_desc())
        return EDEADLK;
    if (t->detached)
        return EINVAL;
    for (;;) {
        uint32_t v = __atomic_load_n(&t->tid, __ATOMIC_ACQUIRE);
        if (v == 0)
            break;
        futex_wait(&t->tid, v, -1);
    }
    if (ret)
        *ret = t->ret;
    if (t != &main_thread)
        free_thread(t);
    return 0;
}

int pthread_detach(pthread_t th)
{
    struct pthread *t = (struct pthread *)th;
    if (t->detached)
        return EINVAL;
    t->detached = 1;
    pthread_mutex_lock(&dead_lock);
    t->next_dead = dead_detached;
    dead_detached = t;
    pthread_mutex_unlock(&dead_lock);
    return 0;
}

int pthread_kill(pthread_t th, int sig)
{
    (void)th;
    return kill(getpid(), sig) < 0 ? errno : 0; /* process-directed */
}

/* ---- attributes ------------------------------------------------------------- */

int pthread_attr_init(pthread_attr_t *a)
{
    memset(a, 0, sizeof *a);
    a->is_initialized = 1;
    a->detachstate = PTHREAD_CREATE_JOINABLE;
    a->stacksize = DEFAULT_STACK;
    return 0;
}
int pthread_attr_destroy(pthread_attr_t *a)
{
    a->is_initialized = 0;
    return 0;
}
int pthread_attr_setdetachstate(pthread_attr_t *a, int s)
{
    if (s != PTHREAD_CREATE_DETACHED && s != PTHREAD_CREATE_JOINABLE)
        return EINVAL;
    a->detachstate = s;
    return 0;
}
int pthread_attr_getdetachstate(const pthread_attr_t *a, int *s)
{
    *s = a->detachstate;
    return 0;
}
int pthread_attr_setstacksize(pthread_attr_t *a, size_t s)
{
    if (s < PTHREAD_STACK_MIN || s > (64u << 20))
        return EINVAL;
    a->stacksize = (int)s;
    return 0;
}
int pthread_attr_getstacksize(const pthread_attr_t *a, size_t *s)
{
    *s = (size_t)a->stacksize;
    return 0;
}

/* ---- mutexes ----------------------------------------------------------------- */

int pthread_mutexattr_init(pthread_mutexattr_t *a)
{
    memset(a, 0, sizeof *a);
    a->is_initialized = 1;
    a->type = PTHREAD_MUTEX_DEFAULT;
    return 0;
}
int pthread_mutexattr_destroy(pthread_mutexattr_t *a)
{
    a->is_initialized = 0;
    return 0;
}
int pthread_mutexattr_settype(pthread_mutexattr_t *a, int type)
{
    if (type < PTHREAD_MUTEX_NORMAL || type > PTHREAD_MUTEX_DEFAULT)
        return EINVAL;
    a->type = type;
    return 0;
}
int pthread_mutexattr_gettype(const pthread_mutexattr_t *a, int *type)
{
    *type = a->type;
    return 0;
}

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a)
{
    m->lock = 0;
    m->owner = 0;
    m->count = 0;
    m->type = a ? a->type : PTHREAD_MUTEX_DEFAULT;
    return 0;
}
int pthread_mutex_destroy(pthread_mutex_t *m) { return m->lock ? EBUSY : 0; }

static void lock_word(volatile unsigned int *w)
{
    unsigned int c = 0;
    if (__atomic_compare_exchange_n(w, &c, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return;
    /* Contended: mark it (2) and sleep until the holder wakes us. */
    if (c != 2)
        c = __atomic_exchange_n(w, 2, __ATOMIC_ACQUIRE);
    while (c != 0) {
        futex_wait((volatile uint32_t *)w, 2, -1);
        c = __atomic_exchange_n(w, 2, __ATOMIC_ACQUIRE);
    }
}

static void unlock_word(volatile unsigned int *w)
{
    if (__atomic_fetch_sub(w, 1, __ATOMIC_RELEASE) != 1) {
        __atomic_store_n(w, 0, __ATOMIC_RELEASE);
        futex_wake((volatile uint32_t *)w, 1);
    }
}

int pthread_mutex_lock(pthread_mutex_t *m)
{
    if (m->type == PTHREAD_MUTEX_RECURSIVE || m->type == PTHREAD_MUTEX_ERRORCHECK) {
        int me = self_tid();
        if (m->owner == me) {
            if (m->type == PTHREAD_MUTEX_ERRORCHECK)
                return EDEADLK;
            m->count++;
            return 0;
        }
        lock_word(&m->lock);
        m->owner = me;
        m->count = 1;
        return 0;
    }
    lock_word(&m->lock);
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *m)
{
    int me = self_tid();
    if (m->type == PTHREAD_MUTEX_RECURSIVE && m->owner == me) {
        m->count++;
        return 0;
    }
    unsigned int c = 0;
    if (!__atomic_compare_exchange_n(&m->lock, &c, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return EBUSY;
    if (m->type == PTHREAD_MUTEX_RECURSIVE || m->type == PTHREAD_MUTEX_ERRORCHECK) {
        m->owner = me;
        m->count = 1;
    }
    return 0;
}

int pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec *abstime)
{
    (void)abstime;
    return pthread_mutex_lock(m); /* timeouts are not implemented for mutexes */
}

int pthread_mutex_unlock(pthread_mutex_t *m)
{
    if (m->type == PTHREAD_MUTEX_RECURSIVE || m->type == PTHREAD_MUTEX_ERRORCHECK) {
        if (m->owner != self_tid())
            return EPERM;
        if (--m->count > 0)
            return 0;
        m->owner = 0;
    }
    unlock_word(&m->lock);
    return 0;
}

/* ---- condition variables ------------------------------------------------------ */

int pthread_condattr_init(pthread_condattr_t *a)
{
    memset(a, 0, sizeof *a);
    a->is_initialized = 1;
    a->clock = CLOCK_REALTIME;
    return 0;
}
int pthread_condattr_destroy(pthread_condattr_t *a)
{
    a->is_initialized = 0;
    return 0;
}
int pthread_condattr_setclock(pthread_condattr_t *a, clockid_t c)
{
    a->clock = (int)c;
    return 0;
}

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a)
{
    c->seq = 0;
    c->clock = a ? a->clock : CLOCK_REALTIME;
    return 0;
}
int pthread_cond_destroy(pthread_cond_t *c)
{
    (void)c;
    return 0;
}

static int cond_wait(pthread_cond_t *c, pthread_mutex_t *m, int64_t timeout_ns)
{
    uint32_t seq = __atomic_load_n(&c->seq, __ATOMIC_ACQUIRE);
    int type = m->type;
    int count = m->count;
    if (type == PTHREAD_MUTEX_RECURSIVE || type == PTHREAD_MUTEX_ERRORCHECK) {
        m->count = 1;
    }
    pthread_mutex_unlock(m);
    long r = futex_wait(&c->seq, seq, timeout_ns);
    pthread_mutex_lock(m);
    if (type == PTHREAD_MUTEX_RECURSIVE)
        m->count = count;
    return r == -ETIMEDOUT ? ETIMEDOUT : 0;
}

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) { return cond_wait(c, m, -1); }

int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *abstime)
{
    struct timespec now;
    clock_gettime(c->clock == CLOCK_MONOTONIC ? CLOCK_MONOTONIC : CLOCK_REALTIME, &now);
    int64_t ns = (abstime->tv_sec - now.tv_sec) * 1000000000ll + (abstime->tv_nsec - now.tv_nsec);
    if (ns <= 0)
        return ETIMEDOUT;
    return cond_wait(c, m, ns);
}

int pthread_cond_signal(pthread_cond_t *c)
{
    __atomic_add_fetch(&c->seq, 1, __ATOMIC_RELEASE);
    futex_wake(&c->seq, 1);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *c)
{
    __atomic_add_fetch(&c->seq, 1, __ATOMIC_RELEASE);
    futex_wake(&c->seq, 0x7fffffff);
    return 0;
}

/* ---- once, rwlocks, spinlocks, barriers -------------------------------------- */

int pthread_once(pthread_once_t *o, void (*fn)(void))
{
    for (;;) {
        int s = __atomic_load_n(&o->init_executed, __ATOMIC_ACQUIRE);
        if (s == 2)
            return 0;
        if (s == 0) {
            int z = 0;
            if (__atomic_compare_exchange_n(&o->init_executed, &z, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                fn();
                __atomic_store_n(&o->init_executed, 2, __ATOMIC_RELEASE);
                futex_wake((volatile uint32_t *)&o->init_executed, 0x7fffffff);
                return 0;
            }
            continue;
        }
        futex_wait((volatile uint32_t *)&o->init_executed, 1, -1);
    }
}

int pthread_rwlock_init(pthread_rwlock_t *l, const pthread_rwlockattr_t *a)
{
    (void)a;
    memset(l, 0, sizeof *l);
    l->lock.type = PTHREAD_MUTEX_NORMAL;
    return 0;
}
int pthread_rwlock_destroy(pthread_rwlock_t *l) { return l->readers || l->writer ? EBUSY : 0; }

int pthread_rwlock_rdlock(pthread_rwlock_t *l)
{
    pthread_mutex_lock(&l->lock);
    while (l->writer || l->waiting_writers)
        pthread_cond_wait(&l->readers_ok, &l->lock);
    l->readers++;
    pthread_mutex_unlock(&l->lock);
    return 0;
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *l)
{
    pthread_mutex_lock(&l->lock);
    int r = (l->writer || l->waiting_writers) ? EBUSY : (l->readers++, 0);
    pthread_mutex_unlock(&l->lock);
    return r;
}

int pthread_rwlock_wrlock(pthread_rwlock_t *l)
{
    pthread_mutex_lock(&l->lock);
    l->waiting_writers++;
    while (l->writer || l->readers)
        pthread_cond_wait(&l->writers_ok, &l->lock);
    l->waiting_writers--;
    l->writer = 1;
    pthread_mutex_unlock(&l->lock);
    return 0;
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *l)
{
    pthread_mutex_lock(&l->lock);
    int r = (l->writer || l->readers) ? EBUSY : (l->writer = 1, 0);
    pthread_mutex_unlock(&l->lock);
    return r;
}

int pthread_rwlock_unlock(pthread_rwlock_t *l)
{
    pthread_mutex_lock(&l->lock);
    if (l->writer)
        l->writer = 0;
    else if (l->readers)
        l->readers--;
    if (l->waiting_writers && !l->readers)
        pthread_cond_signal(&l->writers_ok);
    else if (!l->waiting_writers)
        pthread_cond_broadcast(&l->readers_ok);
    pthread_mutex_unlock(&l->lock);
    return 0;
}

int pthread_spin_init(pthread_spinlock_t *s, int pshared)
{
    (void)pshared;
    *s = 0;
    return 0;
}
int pthread_spin_destroy(pthread_spinlock_t *s)
{
    (void)s;
    return 0;
}
int pthread_spin_lock(pthread_spinlock_t *s)
{
    while (__atomic_exchange_n(s, 1, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(s, __ATOMIC_RELAXED))
            __builtin_ia32_pause();
    return 0;
}
int pthread_spin_trylock(pthread_spinlock_t *s) { return __atomic_exchange_n(s, 1, __ATOMIC_ACQUIRE) ? EBUSY : 0; }
int pthread_spin_unlock(pthread_spinlock_t *s)
{
    __atomic_store_n(s, 0, __ATOMIC_RELEASE);
    return 0;
}

int pthread_barrier_init(pthread_barrier_t *b, const pthread_barrierattr_t *a, unsigned count)
{
    (void)a;
    if (!count)
        return EINVAL;
    memset(b, 0, sizeof *b);
    b->count = count;
    return 0;
}
int pthread_barrier_destroy(pthread_barrier_t *b) { return b->waiting ? EBUSY : 0; }
int pthread_barrier_wait(pthread_barrier_t *b)
{
    pthread_mutex_lock(&b->lock);
    unsigned cycle = b->cycle;
    if (++b->waiting == b->count) {
        b->waiting = 0;
        b->cycle++;
        pthread_cond_broadcast(&b->cond);
        pthread_mutex_unlock(&b->lock);
        return PTHREAD_BARRIER_SERIAL_THREAD;
    }
    while (cycle == b->cycle)
        pthread_cond_wait(&b->cond, &b->lock);
    pthread_mutex_unlock(&b->lock);
    return 0;
}

int pthread_setcancelstate(int state, int *old)
{
    (void)state;
    if (old)
        *old = 0;
    return 0; /* cancellation is not implemented */
}

int pthread_getcpuclockid(pthread_t th, clockid_t *c)
{
    (void)th;
    *c = CLOCK_THREAD_CPUTIME_ID;
    return 0;
}

int pthread_attr_setscope(pthread_attr_t *a, int s)
{
    a->contentionscope = s;
    return 0;
}
