/* Users and groups (a fixed table: Aurora is single-user by default with a
 * root account and an unprivileged "user" account), login name, and
 * setjmp aliases. */
#include "sys.h"
#include <pwd.h>
#include <grp.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

static struct passwd users[] = {
    { .pw_name = "root", .pw_passwd = "x", .pw_uid = 0, .pw_gid = 0, .pw_comment = "",
      .pw_gecos = "System Administrator", .pw_dir = "/home", .pw_shell = "/system/bin/sh" },
    { .pw_name = "user", .pw_passwd = "x", .pw_uid = 1000, .pw_gid = 1000, .pw_comment = "",
      .pw_gecos = "Aurora User", .pw_dir = "/home", .pw_shell = "/system/bin/sh" },
};
static int pw_index;

struct passwd *getpwuid(uid_t uid)
{
    for (size_t i = 0; i < sizeof users / sizeof users[0]; i++)
        if (users[i].pw_uid == uid)
            return &users[i];
    return NULL;
}

struct passwd *getpwnam(const char *name)
{
    for (size_t i = 0; i < sizeof users / sizeof users[0]; i++)
        if (!strcmp(users[i].pw_name, name))
            return &users[i];
    return NULL;
}

struct passwd *getpwent(void) { return pw_index < 2 ? &users[pw_index++] : NULL; }
void setpwent(void) { pw_index = 0; }
void endpwent(void) { pw_index = 0; }

static char *root_members[] = { "root", NULL };
static char *user_members[] = { "user", NULL };
static struct group groups[] = {
    { "root", "x", 0, root_members },
    { "user", "x", 1000, user_members },
};

struct group *getgrgid(gid_t gid)
{
    for (size_t i = 0; i < 2; i++)
        if (groups[i].gr_gid == gid)
            return &groups[i];
    return NULL;
}

struct group *getgrnam(const char *name)
{
    for (size_t i = 0; i < 2; i++)
        if (!strcmp(groups[i].gr_name, name))
            return &groups[i];
    return NULL;
}

char *getlogin(void)
{
    struct passwd *p = getpwuid(getuid());
    return p ? p->pw_name : NULL;
}

/* POSIX 2008 names for newlib's __getline/__getdelim. */
#include <stdio.h>
ssize_t getline(char **p, size_t *n, FILE *f) { return __getline(p, n, f); }
ssize_t getdelim(char **p, size_t *n, int d, FILE *f) { return __getdelim(p, n, d, f); }

/* errno lives in newlib's thread-local _tls_errno (_REENT_THREAD_LOCAL);
 * <errno.h> still reaches it through __errno(). */
extern _Thread_local int _tls_errno;
int *__errno(void) { return &_tls_errno; }

/* No set-user-ID programs exist on Aurora. */
int issetugid(void) { return 0; }

/* POSIX stdio locking on newlib's per-FILE recursive locks. */
#include <sys/lock.h>
void flockfile(FILE *f) { _flockfile(f); }
void funlockfile(FILE *f) { _funlockfile(f); }
int ftrylockfile(FILE *f)
{
    if (f->_flags & __SSTR)
        return 0;
    return __lock_try_acquire_recursive(f->_lock) ? 0 : -1;
}
