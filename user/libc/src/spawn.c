/* posix_spawn on the native SYS_SPAWN call (no fork), and system().
 *
 * File actions are applied to a descriptor map: dup2 and close rewrite the
 * map, open actions open the file in the parent (close-on-exec) and map it.
 * The kernel then gives the child exactly the mapped descriptors. */
#include "sys.h"
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <paths.h>

#define MAX_ACTIONS 32

enum { ACT_OPEN, ACT_CLOSE, ACT_DUP2 };

struct action {
    int kind, fd, src, flags;
    mode_t mode;
    char *path;
};

struct __posix_spawn_file_actions {
    int n;
    struct action a[MAX_ACTIONS];
};

struct __posix_spawnattr {
    short flags;
    pid_t pgroup;
    sigset_t mask, def;
};

int posix_spawn_file_actions_init(posix_spawn_file_actions_t *fa)
{
    *fa = calloc(1, sizeof **fa);
    return *fa ? 0 : ENOMEM;
}

int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *fa)
{
    if (*fa) {
        for (int i = 0; i < (*fa)->n; i++)
            free((*fa)->a[i].path);
        free(*fa);
        *fa = NULL;
    }
    return 0;
}

static struct action *add(posix_spawn_file_actions_t *fa)
{
    if (!*fa || (*fa)->n >= MAX_ACTIONS)
        return NULL;
    return &(*fa)->a[(*fa)->n++];
}

int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *__restrict fa, int fd, const char *__restrict path,
                                     int flags, mode_t mode)
{
    if (fd < 0 || fd >= AURORA_SPAWN_FDS)
        return EBADF;
    struct action *a = add(fa);
    if (!a)
        return ENOMEM;
    a->kind = ACT_OPEN;
    a->fd = fd;
    a->flags = flags;
    a->mode = mode;
    a->path = strdup(path);
    return a->path ? 0 : ENOMEM;
}

int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *fa, int src, int fd)
{
    if (fd < 0 || fd >= AURORA_SPAWN_FDS || src < 0)
        return EBADF;
    struct action *a = add(fa);
    if (!a)
        return ENOMEM;
    a->kind = ACT_DUP2;
    a->src = src;
    a->fd = fd;
    return 0;
}

int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *fa, int fd)
{
    if (fd < 0 || fd >= AURORA_SPAWN_FDS)
        return EBADF;
    struct action *a = add(fa);
    if (!a)
        return ENOMEM;
    a->kind = ACT_CLOSE;
    a->fd = fd;
    return 0;
}

int posix_spawnattr_init(posix_spawnattr_t *at)
{
    *at = calloc(1, sizeof **at);
    return *at ? 0 : ENOMEM;
}
int posix_spawnattr_destroy(posix_spawnattr_t *at)
{
    free(*at);
    *at = NULL;
    return 0;
}
int posix_spawnattr_setflags(posix_spawnattr_t *at, short f)
{
    (*at)->flags = f;
    return 0;
}
int posix_spawnattr_getflags(const posix_spawnattr_t *__restrict at, short *__restrict f)
{
    *f = (*at)->flags;
    return 0;
}
int posix_spawnattr_setpgroup(posix_spawnattr_t *at, pid_t g)
{
    (*at)->pgroup = g;
    return 0;
}
int posix_spawnattr_getpgroup(const posix_spawnattr_t *__restrict at, pid_t *__restrict g)
{
    *g = (*at)->pgroup;
    return 0;
}
int posix_spawnattr_setsigmask(posix_spawnattr_t *__restrict at, const sigset_t *__restrict m)
{
    (*at)->mask = *m;
    return 0;
}
int posix_spawnattr_getsigmask(const posix_spawnattr_t *__restrict at, sigset_t *__restrict m)
{
    *m = (*at)->mask;
    return 0;
}
int posix_spawnattr_setsigdefault(posix_spawnattr_t *__restrict at, const sigset_t *__restrict m)
{
    (*at)->def = *m;
    return 0;
}
int posix_spawnattr_getsigdefault(const posix_spawnattr_t *__restrict at, sigset_t *__restrict m)
{
    *m = (*at)->def;
    return 0;
}

int posix_spawn(pid_t *__restrict pid, const char *__restrict path, const posix_spawn_file_actions_t *fa,
                const posix_spawnattr_t *__restrict at, char *const argv[], char *const envp[])
{
    struct aurora_spawn s;
    memset(&s, 0, sizeof s);
    s.nfds = AURORA_SPAWN_FDS;
    /* Start from the descriptors a fork+exec child would keep. */
    for (int i = 0; i < AURORA_SPAWN_FDS; i++) {
        int fl = fcntl(i, F_GETFD);
        s.fd_map[i] = (fl >= 0 && !(fl & FD_CLOEXEC)) ? i : -1;
    }
    int opened[MAX_ACTIONS], nopen = 0;
    int err = 0;
    /* set[i]: slot i was rewritten by an action (otherwise a dup2 source
     * means the parent's descriptor, as it would before exec). */
    char set[AURORA_SPAWN_FDS] = { 0 };
    if (fa && *fa) {
        for (int k = 0; k < (*fa)->n && !err; k++) {
            const struct action *a = &(*fa)->a[k];
            if (a->kind == ACT_CLOSE) {
                s.fd_map[a->fd] = -1;
                set[a->fd] = 1;
            } else if (a->kind == ACT_DUP2) {
                int from = (a->src < AURORA_SPAWN_FDS && set[a->src]) ? s.fd_map[a->src] : a->src;
                if (from < 0 || fcntl(from, F_GETFD) < 0) {
                    err = EBADF;
                    break;
                }
                s.fd_map[a->fd] = from;
                set[a->fd] = 1;
            } else {
                int fd = open(a->path, a->flags | O_CLOEXEC, a->mode);
                if (fd < 0) {
                    err = errno;
                    break;
                }
                opened[nopen++] = fd;
                s.fd_map[a->fd] = fd;
                set[a->fd] = 1;
            }
        }
    }
    if (at && *at) {
        if ((*at)->flags & POSIX_SPAWN_SETPGROUP) {
            s.flags |= AURORA_SPAWN_SETPGROUP;
            s.pgroup = (*at)->pgroup;
        }
        if ((*at)->flags & POSIX_SPAWN_SETSIGMASK) {
            s.flags |= AURORA_SPAWN_SETSIGMASK;
            s.sigmask = (uint64_t)(*at)->mask;
        }
        if ((*at)->flags & POSIX_SPAWN_SETSIGDEF) {
            s.flags |= AURORA_SPAWN_SETSIGDEF;
            s.sigdefault = (uint64_t)(*at)->def;
        }
    }
    long r = err ? -err : __sys4(SYS_SPAWN, path, argv, envp, &s);
    for (int i = 0; i < nopen; i++)
        close(opened[i]);
    if (r < 0)
        return (int)-r;
    if (pid)
        *pid = (pid_t)r;
    return 0;
}

int posix_spawnp(pid_t *__restrict pid, const char *__restrict file, const posix_spawn_file_actions_t *fa,
                 const posix_spawnattr_t *__restrict at, char *const argv[], char *const envp[])
{
    if (strchr(file, '/'))
        return posix_spawn(pid, file, fa, at, argv, envp);
    const char *path = getenv("PATH");
    if (!path || !*path)
        path = _PATH_DEFPATH;
    char buf[256];
    int last = ENOENT;
    while (*path) {
        const char *e = strchr(path, ':');
        size_t n = e ? (size_t)(e - path) : strlen(path);
        if (n + 1 + strlen(file) + 1 <= sizeof buf) {
            memcpy(buf, path, n);
            buf[n] = '/';
            strcpy(buf + n + 1, file);
            int r = posix_spawn(pid, buf, fa, at, argv, envp);
            if (r == 0)
                return 0;
            if (r != ENOENT)
                last = r;
        }
        path = e ? e + 1 : path + n;
    }
    return last;
}

int system(const char *cmd)
{
    if (!cmd)
        return access(_PATH_BSHELL, X_OK) == 0;
    char *argv[] = { "sh", "-c", (char *)cmd, NULL };
    pid_t pid;
    struct sigaction ign, oint, oquit;
    memset(&ign, 0, sizeof ign);
    ign.sa_handler = SIG_IGN;
    sigaction(SIGINT, &ign, &oint);
    sigaction(SIGQUIT, &ign, &oquit);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    sigset_t def;
    sigemptyset(&def);
    sigaddset(&def, SIGINT);
    sigaddset(&def, SIGQUIT);
    posix_spawnattr_setsigdefault(&at, &def);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGDEF);
    int r = posix_spawn(&pid, _PATH_BSHELL, NULL, &at, argv, environ);
    posix_spawnattr_destroy(&at);
    int status = -1;
    if (r == 0) {
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
            ;
    } else {
        errno = r;
        status = 127 << 8;
    }
    sigaction(SIGINT, &oint, NULL);
    sigaction(SIGQUIT, &oquit, NULL);
    return status;
}
