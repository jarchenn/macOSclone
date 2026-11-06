/* newlib's retargetable locks (stdio, malloc, atexit, environment, time
 * zone) implemented with Aurora's futex mutexes. */
#include <sys/lock.h>
#include <pthread.h>
#include <stdlib.h>

struct __lock {
    pthread_mutex_t m;
};

#define RECURSIVE { _PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP }
#define NORMAL { _PTHREAD_MUTEX_INITIALIZER }

struct __lock __lock___sinit_recursive_mutex = RECURSIVE;
struct __lock __lock___sfp_recursive_mutex = RECURSIVE;
struct __lock __lock___atexit_recursive_mutex = RECURSIVE;
struct __lock __lock___at_quick_exit_mutex = NORMAL;
struct __lock __lock___malloc_recursive_mutex = RECURSIVE;
struct __lock __lock___env_recursive_mutex = RECURSIVE;
struct __lock __lock___tz_mutex = NORMAL;
struct __lock __lock___dd_hash_mutex = NORMAL;
struct __lock __lock___arc4random_mutex = NORMAL;

static void init(_LOCK_T *l, int type)
{
    struct __lock *k = malloc(sizeof *k);
    if (!k)
        abort();
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, type);
    pthread_mutex_init(&k->m, &a);
    *l = k;
}

void __retarget_lock_init(_LOCK_T *l) { init(l, PTHREAD_MUTEX_NORMAL); }
void __retarget_lock_init_recursive(_LOCK_T *l) { init(l, PTHREAD_MUTEX_RECURSIVE); }
void __retarget_lock_close(_LOCK_T l) { free(l); }
void __retarget_lock_close_recursive(_LOCK_T l) { free(l); }
void __retarget_lock_acquire(_LOCK_T l) { pthread_mutex_lock(&l->m); }
void __retarget_lock_acquire_recursive(_LOCK_T l) { pthread_mutex_lock(&l->m); }
int __retarget_lock_try_acquire(_LOCK_T l) { return pthread_mutex_trylock(&l->m) == 0; }
int __retarget_lock_try_acquire_recursive(_LOCK_T l) { return pthread_mutex_trylock(&l->m) == 0; }
void __retarget_lock_release(_LOCK_T l) { pthread_mutex_unlock(&l->m); }
void __retarget_lock_release_recursive(_LOCK_T l) { pthread_mutex_unlock(&l->m); }
