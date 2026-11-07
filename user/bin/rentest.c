/* rentest setup | check: rename and unlink results must persist across a
 * clean reboot (0.6 acceptance). `setup` builds a known state in
 * /home/rentest with rename (including over an existing file), unlink,
 * mkdir/rmdir and truncate, then syncs; `check` (after the reboot)
 * verifies it. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int put(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    ssize_t n = write(fd, text, strlen(text));
    close(fd);
    return n == (ssize_t)strlen(text) ? 0 : -1;
}

static int content_is(const char *path, const char *want)
{
    char buf[128] = { 0 };
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    return n == (ssize_t)strlen(want) && !memcmp(buf, want, (size_t)n);
}

static int missing(const char *path)
{
    struct stat st;
    return stat(path, &st) < 0 && errno == ENOENT;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: rentest setup|check\n");
        return 2;
    }
    if (!strcmp(argv[1], "setup")) {
        mkdir("/home/rentest", 0755);
        int r = 0;
        r |= put("/home/rentest/a.txt", "alpha\n");
        r |= rename("/home/rentest/a.txt", "/home/rentest/b.txt");
        r |= put("/home/rentest/c.txt", "gone\n");
        r |= unlink("/home/rentest/c.txt");
        r |= put("/home/rentest/old.txt", "old\n");
        r |= put("/home/rentest/new.txt", "new contents\n");
        r |= rename("/home/rentest/new.txt", "/home/rentest/old.txt"); /* replaces old.txt */
        r |= mkdir("/home/rentest/dir", 0755);
        r |= rename("/home/rentest/dir", "/home/rentest/moved");
        r |= mkdir("/home/rentest/tmpdir", 0755);
        r |= rmdir("/home/rentest/tmpdir");
        r |= put("/home/rentest/trunc.txt", "0123456789");
        r |= truncate("/home/rentest/trunc.txt", 4);
        sync();
        printf("rentest: setup %s\n", r ? "FAILED" : "done");
        return r ? 1 : 0;
    }
    int ok = content_is("/home/rentest/b.txt", "alpha\n") && missing("/home/rentest/a.txt") &&
             missing("/home/rentest/c.txt") && content_is("/home/rentest/old.txt", "new contents\n") &&
             missing("/home/rentest/new.txt") && missing("/home/rentest/dir") && missing("/home/rentest/tmpdir") &&
             content_is("/home/rentest/trunc.txt", "0123");
    struct stat st;
    ok = ok && stat("/home/rentest/moved", &st) == 0 && S_ISDIR(st.st_mode);
    printf("rentest: check %s\n", ok ? "passed" : "FAILED");
    return ok ? 0 : 1;
}
