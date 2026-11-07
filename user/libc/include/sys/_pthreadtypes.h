/* POSIX thread types for Aurora's pthread implementation (user/libc/src/
 * pthread.c). Replaces newlib's generic <sys/_pthreadtypes.h>, whose
 * 32-bit object handles suit RTEMS but not a futex-based library. */
#ifndef _SYS__PTHREADTYPES_H_
#define _SYS__PTHREADTYPES_H_

#if defined(_POSIX_THREADS) || __POSIX_VISIBLE >= 199506

#include <sys/sched.h>

typedef unsigned long pthread_t; /* address of the thread's descriptor */

#define PTHREAD_SCOPE_PROCESS 0
#define PTHREAD_SCOPE_SYSTEM 1
#define PTHREAD_INHERIT_SCHED 1
#define PTHREAD_EXPLICIT_SCHED 2
#define PTHREAD_CREATE_DETACHED 0
#define PTHREAD_CREATE_JOINABLE 1
#define PTHREAD_STACK_MIN 16384
#define PTHREAD_PROCESS_PRIVATE 0
#define PTHREAD_PROCESS_SHARED 1

typedef struct {
    int is_initialized;
    void *stackaddr;
    int stacksize;
    int contentionscope;
    int inheritsched;
    int schedpolicy;
    struct sched_param schedparam;
    int detachstate;
} pthread_attr_t;

#define PTHREAD_MUTEX_NORMAL 0
#define PTHREAD_MUTEX_RECURSIVE 1
#define PTHREAD_MUTEX_ERRORCHECK 2
#define PTHREAD_MUTEX_DEFAULT 3

/* lock: 0 free, 1 held, 2 held with waiters (futex word). */
typedef struct {
    volatile unsigned int lock;
    int type;
    volatile int owner; /* thread id, for recursive and error-checking types */
    int count;
} pthread_mutex_t;

typedef struct {
    int is_initialized;
    int type;
    int process_shared;
    int recursive;
} pthread_mutexattr_t;

#define _PTHREAD_MUTEX_INITIALIZER { 0, PTHREAD_MUTEX_DEFAULT, 0, 0 }
#define _PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP { 0, PTHREAD_MUTEX_RECURSIVE, 0, 0 }

typedef struct {
    volatile unsigned int seq; /* futex word: bumped by signal/broadcast */
    int clock;
} pthread_cond_t;

typedef struct {
    int is_initialized;
    int clock;
    int process_shared;
} pthread_condattr_t;

#define _PTHREAD_COND_INITIALIZER { 0, 0 }

typedef unsigned int pthread_key_t;

typedef struct {
    int is_initialized;
    volatile int init_executed; /* 0 not run, 1 running, 2 done */
} pthread_once_t;

#define _PTHREAD_ONCE_INIT { 1, 0 }

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t readers_ok, writers_ok;
    int readers, writer, waiting_writers;
} pthread_rwlock_t;

typedef struct {
    int is_initialized;
    int process_shared;
} pthread_rwlockattr_t;

#define _PTHREAD_RWLOCK_INITIALIZER { _PTHREAD_MUTEX_INITIALIZER, _PTHREAD_COND_INITIALIZER, _PTHREAD_COND_INITIALIZER, 0, 0, 0 }

typedef volatile unsigned int pthread_spinlock_t;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    unsigned int count, waiting, cycle;
} pthread_barrier_t;

typedef struct {
    int is_initialized;
    int process_shared;
} pthread_barrierattr_t;

#endif
#endif
