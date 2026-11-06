/* permtest: permission checks (0.6 acceptance). As root it creates a
 * root-owned file with mode 0600, then a child drops to uid 1000 and must
 * be refused (EACCES) for reading and writing it; it also cannot execute a
 * file without execute bits, chmod someone else's file, become root again,
 * or create files in a directory it may not write. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

extern char **environ;

static int child(void)
{
    int bad = 0;
#define EXPECT(what, cond)                                       \
    do {                                                         \
        int ok_ = (cond);                                        \
        printf("permtest: %s %s\n", ok_ ? "ok  " : "FAIL", what); \
        bad += !ok_;                                             \
    } while (0)
    EXPECT("setuid(1000)", setuid(1000) == 0 && getuid() == 1000);
    errno = 0;
    EXPECT("non-root cannot open a root 0600 file for reading", open("/home/permtest/secret", O_RDONLY) < 0 && errno == EACCES);
    errno = 0;
    EXPECT("... or for writing", open("/home/permtest/secret", O_WRONLY) < 0 && errno == EACCES);
    errno = 0;
    EXPECT("access(R_OK) agrees", access("/home/permtest/secret", R_OK) < 0 && errno == EACCES);
    errno = 0;
    EXPECT("chmod of a file owned by root: EPERM", chmod("/home/permtest/secret", 0666) < 0 && errno == EPERM);
    errno = 0;
    char *argv[] = { "script", NULL };
    EXPECT("exec without execute permission: EACCES", execve("/home/permtest/noexec", argv, environ) < 0 && errno == EACCES);
    errno = 0;
    EXPECT("create in a root 0755 directory: EACCES", open("/home/permtest/new", O_WRONLY | O_CREAT, 0644) < 0 && errno == EACCES);
    EXPECT("world-readable root file is readable", open("/home/permtest/public", O_RDONLY) >= 0);
    errno = 0;
    EXPECT("setuid(0) back to root: EPERM", setuid(0) < 0 && errno == EPERM);
    int fd = open("/tmp/permtest-user", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    struct stat st;
    EXPECT("files created in /tmp belong to uid 1000", fd >= 0 && fstat(fd, &st) == 0 && st.st_uid == 1000);
    close(fd);
    unlink("/tmp/permtest-user");
    return bad;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (getuid() != 0) {
        printf("permtest: must start as root\n");
        return 2;
    }
    mkdir("/home/permtest", 0755);
    chmod("/home/permtest", 0755);
    int fd = open("/home/permtest/secret", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    write(fd, "top secret\n", 11);
    close(fd);
    chmod("/home/permtest/secret", 0600);
    fd = open("/home/permtest/public", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    close(fd);
    fd = open("/home/permtest/noexec", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    close(fd);
    struct stat st;
    int ok = stat("/home/permtest/secret", &st) == 0 && st.st_uid == 0 && (st.st_mode & 0777) == 0600;
    printf("permtest: %s root owns secret with mode %o\n", ok ? "ok  " : "FAIL", (unsigned)(st.st_mode & 0777));
    int rfd = open("/home/permtest/secret", O_RDONLY);
    printf("permtest: %s root can still read it\n", rfd >= 0 ? "ok  " : "FAIL");
    close(rfd);
    pid_t p = fork();
    if (p == 0)
        _exit(child());
    int status = 0;
    waitpid(p, &status, 0);
    int fails = (WIFEXITED(status) ? WEXITSTATUS(status) : 99) + !ok + (rfd < 0);
    printf("permtest: %s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
