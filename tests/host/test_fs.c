/* Path normalisation, VFS semantics and initramfs validation. */
#include "harness.h"
#include "fs/vfs.h"
#include "fs/initramfs.h"
#include "lib.h"
#include <stdlib.h>

const char *t_suite = "fs";

static int norm(const char *cwd, const char *in, char *out) { return vfs_normalize(cwd, in, out, VFS_PATH_MAX + 1); }

TEST(normalize_rules)
{
    char o[VFS_PATH_MAX + 1];
    CHECK_EQ(norm("/", "/", o), VFS_OK);
    CHECK_STR(o, "/");
    CHECK_EQ(norm("/", "//home///notes/", o), VFS_OK);
    CHECK_STR(o, "/home/notes");
    CHECK_EQ(norm("/home", "notes/./a.txt", o), VFS_OK);
    CHECK_STR(o, "/home/notes/a.txt");
    CHECK_EQ(norm("/home/notes", "..", o), VFS_OK);
    CHECK_STR(o, "/home");
    CHECK_EQ(norm("/", "../../..", o), VFS_OK); /* .. at root stays at root */
    CHECK_STR(o, "/");
    CHECK_EQ(norm("/home", "/system/../home/x", o), VFS_OK);
    CHECK_STR(o, "/home/x");
    CHECK_EQ(norm("/", "", o), VFS_EINVAL);
    CHECK_EQ(norm("/", "/bad name", o), VFS_EINVAL);
    CHECK_EQ(norm("/", "/bad*char", o), VFS_EINVAL);
    CHECK_EQ(norm("relative", "x", o), VFS_EINVAL); /* cwd must be absolute */
    char longc[80];
    memset(longc, 'a', 64);
    longc[64] = 0;
    CHECK_EQ(norm("/", longc, o), VFS_ENAMETOOLONG); /* 64-char component */
    char longp[400] = "";
    for (int i = 0; i < 40; i++)
        strcat(longp, "/abcdefg");
    CHECK_EQ(norm("/", longp, o), VFS_ENAMETOOLONG); /* 320 bytes */
    char parent[VFS_PATH_MAX + 1], name[VFS_NAME_MAX + 1];
    CHECK_EQ(vfs_split("/home/notes/a.txt", parent, sizeof parent, name, sizeof name), VFS_OK);
    CHECK_STR(parent, "/home/notes");
    CHECK_STR(name, "a.txt");
    CHECK_EQ(vfs_split("/x", parent, sizeof parent, name, sizeof name), VFS_OK);
    CHECK_STR(parent, "/");
    CHECK_EQ(vfs_split("/", parent, sizeof parent, name, sizeof name), VFS_EINVAL);
}

/* Builds an AURFS archive in memory (mirrors tools/make_initramfs.py). */
struct ent {
    uint32_t type;
    const char *path;
    const char *data;
};

static size_t build(uint8_t *buf, const struct ent *e, int n)
{
    size_t off = 32;
    for (int i = 0; i < n; i++) {
        uint32_t pl = (uint32_t)strlen(e[i].path);
        uint64_t dl = e[i].data ? strlen(e[i].data) : 0;
        memcpy(buf + off, &e[i].type, 4);
        memcpy(buf + off + 4, &pl, 4);
        memcpy(buf + off + 8, &dl, 8);
        off += 16;
        memset(buf + off, 0, ALIGN_UP(pl, 8));
        memcpy(buf + off, e[i].path, pl);
        off += ALIGN_UP(pl, 8);
        memset(buf + off, 0, ALIGN_UP(dl, 8));
        if (dl)
            memcpy(buf + off, e[i].data, dl);
        off += ALIGN_UP(dl, 8);
    }
    memcpy(buf, "AURFS01\0", 8);
    uint32_t v = 1, cnt = (uint32_t)n, crc = crc32(buf + 32, off - 32), z = 0;
    uint64_t total = off;
    memcpy(buf + 8, &v, 4);
    memcpy(buf + 12, &cnt, 4);
    memcpy(buf + 16, &total, 8);
    memcpy(buf + 24, &crc, 4);
    memcpy(buf + 28, &z, 4);
    return off;
}

static void refresh_crc(uint8_t *buf, size_t total)
{
    uint32_t crc = crc32(buf + 32, total - 32);
    memcpy(buf + 24, &crc, 4);
}

static const struct ent good[] = {
    { 1, "system", NULL },
    { 2, "system/welcome.txt", "hello aurora\n" },
    { 1, "system/sub", NULL },
    { 2, "system/sub/empty.txt", "" },
    { 1, "home/notes", NULL },
    { 2, "home/notes/seed.txt", "seed" },
};

static int count_cb(void *ctx, uint32_t type, const char *path, const uint8_t *d, uint64_t len)
{
    (void)type; (void)path; (void)d; (void)len;
    (*(int *)ctx)++;
    return VFS_OK;
}

TEST(initramfs_validation)
{
    static uint8_t buf[4096];
    size_t n = build(buf, good, 6);
    int cnt = 0;
    CHECK_EQ(initramfs_parse(buf, n, count_cb, &cnt), VFS_OK);
    CHECK_EQ(cnt, 6);
    /* truncated archive (declared size > available) */
    CHECK_EQ(initramfs_parse(buf, n - 1, count_cb, &cnt), VFS_EBADARCHIVE);
    CHECK_EQ(initramfs_parse(buf, 16, count_cb, &cnt), VFS_EBADARCHIVE);
    CHECK_EQ(initramfs_parse(NULL, 0, count_cb, &cnt), VFS_EBADARCHIVE);
    /* corrupted payload -> CRC mismatch */
    buf[n - 3] ^= 0x55;
    CHECK_EQ(initramfs_parse(buf, n, count_cb, &cnt), VFS_EBADARCHIVE);
    buf[n - 3] ^= 0x55;
    /* bad magic */
    buf[0] = 'X';
    CHECK_EQ(initramfs_parse(buf, n, count_cb, &cnt), VFS_EBADARCHIVE);
    buf[0] = 'A';
    /* impossible data length in entry 2 (after fixing the CRC) */
    uint64_t huge = 1ull << 40;
    size_t e2 = 32 + 16 + 8; /* entry 1 header + padded "system" */
    memcpy(buf + e2 + 8, &huge, 8);
    refresh_crc(buf, n);
    CHECK_EQ(initramfs_parse(buf, n, count_cb, &cnt), VFS_EBADARCHIVE);
    /* entry count larger than present entries */
    n = build(buf, good, 6);
    uint32_t c = 7;
    memcpy(buf + 12, &c, 4);
    CHECK_EQ(initramfs_parse(buf, n, count_cb, &cnt), VFS_EBADARCHIVE);
    /* unsafe paths */
    const struct ent evil1[] = { { 2, "../etc/passwd", "x" } };
    const struct ent evil2[] = { { 2, "/abs", "x" } };
    const struct ent evil3[] = { { 2, "a//b", "x" } };
    const struct ent evil4[] = { { 2, "a/./b", "x" } };
    const struct ent evil5[] = { { 3, "a", "x" } };
    const struct ent evil6[] = { { 1, "dir", "data-for-dir" } };
    CHECK_EQ(initramfs_parse(buf, build(buf, evil1, 1), count_cb, &cnt), VFS_EBADARCHIVE);
    CHECK_EQ(initramfs_parse(buf, build(buf, evil2, 1), count_cb, &cnt), VFS_EBADARCHIVE);
    CHECK_EQ(initramfs_parse(buf, build(buf, evil3, 1), count_cb, &cnt), VFS_EBADARCHIVE);
    CHECK_EQ(initramfs_parse(buf, build(buf, evil4, 1), count_cb, &cnt), VFS_EBADARCHIVE);
    CHECK_EQ(initramfs_parse(buf, build(buf, evil5, 1), count_cb, &cnt), VFS_EBADARCHIVE);
    CHECK_EQ(initramfs_parse(buf, build(buf, evil6, 1), count_cb, &cnt), VFS_EBADARCHIVE);
    /* trailing garbage within declared size */
    n = build(buf, good, 2);
    uint64_t t2 = n + 8;
    memset(buf + n, 0, 8);
    memcpy(buf + 16, &t2, 8);
    refresh_crc(buf, (size_t)t2);
    CHECK_EQ(initramfs_parse(buf, (size_t)t2, count_cb, &cnt), VFS_EBADARCHIVE);
}

static uint8_t archive[4096];

static void fresh_vfs(void)
{
    vfs_init();
    size_t n = build(archive, good, 6);
    CHECK_EQ(vfs_load_initramfs(archive, n), VFS_OK);
}

TEST(vfs_mounts_and_readonly)
{
    fresh_vfs();
    struct vfs_stat st;
    CHECK_EQ(vfs_stat("/system/welcome.txt", &st), VFS_OK);
    CHECK_EQ(st.type, VFS_FILE);
    CHECK(st.readonly);
    CHECK_EQ(st.size, 13);
    CHECK_EQ(vfs_stat("/home/notes/seed.txt", &st), VFS_OK);
    CHECK(!st.readonly); /* seeded copy in the RAM fs */
    CHECK_EQ(vfs_write_file("/system/welcome.txt", "x", 1), VFS_EROFS);
    CHECK_EQ(vfs_write_file("/system/new.txt", "x", 1), VFS_EROFS);
    CHECK_EQ(vfs_create("/rootfile"), VFS_EROFS);
    CHECK_EQ(vfs_mkdir("/system/d"), VFS_EROFS);
    CHECK_EQ(vfs_truncate("/system/welcome.txt", 0), VFS_EROFS);
    CHECK_EQ(vfs_open("/system/welcome.txt", VFS_O_WRITE), VFS_EROFS);
    char buf[64];
    size_t len;
    CHECK_EQ(vfs_read_file("/system/welcome.txt", buf, sizeof buf, &len), VFS_OK);
    CHECK_EQ(len, 13);
    CHECK(memcmp(buf, "hello aurora\n", 13) == 0);
    /* empty file */
    CHECK_EQ(vfs_read_file("/system/sub/empty.txt", buf, sizeof buf, &len), VFS_OK);
    CHECK_EQ(len, 0);
}

TEST(vfs_errors)
{
    fresh_vfs();
    struct vfs_stat st;
    char buf[8];
    size_t len;
    CHECK_EQ(vfs_stat("/nope", &st), VFS_ENOENT);
    CHECK_EQ(vfs_stat("/system/welcome.txt/x", &st), VFS_ENOTDIR);
    CHECK_EQ(vfs_open("/system", VFS_O_READ), VFS_EISDIR);
    CHECK_EQ(vfs_read_file("/home", buf, sizeof buf, &len), VFS_EISDIR);
    CHECK_EQ(vfs_read_file("/system/welcome.txt", buf, 4, &len), VFS_EFBIG); /* buffer too small */
    CHECK_EQ(vfs_open("/home/missing.txt", VFS_O_READ), VFS_ENOENT);
    CHECK_EQ(vfs_open("/home/x", 0), VFS_EINVAL);
    CHECK_EQ(vfs_read(99, buf, 1), VFS_EBADF);
    CHECK_EQ(vfs_read(-1, buf, 1), VFS_EBADF);
    CHECK_EQ(vfs_close(5), VFS_EBADF);
    CHECK_EQ(vfs_create("/home/notes/seed.txt"), VFS_EEXIST);
    CHECK_EQ(vfs_create("/home/nodir/x.txt"), VFS_ENOENT);
    CHECK_EQ(vfs_create("/home/notes/seed.txt/x"), VFS_ENOTDIR);
    struct vfs_dirent d;
    CHECK_EQ(vfs_readdir("/system/welcome.txt", 0, &d), VFS_ENOTDIR);
    CHECK_EQ(vfs_readdir("/system", 99, &d), VFS_ENOENT);
    CHECK(strcmp(vfs_strerror(VFS_EROFS), "read-only file system") == 0);
}

TEST(vfs_read_write_offsets)
{
    fresh_vfs();
    int fd = vfs_open("/home/f.txt", VFS_O_WRITE | VFS_O_READ | VFS_O_CREATE);
    CHECK(fd >= 0);
    CHECK_EQ(vfs_write(fd, "hello", 5), 5);
    CHECK_EQ(vfs_write(fd, " world", 6), 6);
    CHECK_EQ(vfs_seek(fd, 0), VFS_OK);
    char buf[32] = { 0 };
    CHECK_EQ(vfs_read(fd, buf, 5), 5);
    CHECK(memcmp(buf, "hello", 5) == 0);
    CHECK_EQ(vfs_read(fd, buf, 32), 6);
    CHECK_EQ(vfs_read(fd, buf, 32), 0); /* EOF */
    CHECK_EQ(vfs_seek(fd, 20), VFS_OK);
    CHECK_EQ(vfs_write(fd, "!", 1), 1); /* gap is zero-filled */
    CHECK_EQ(vfs_close(fd), VFS_OK);
    CHECK_EQ(vfs_close(fd), VFS_EBADF);
    struct vfs_stat st;
    CHECK_EQ(vfs_stat("/home/f.txt", &st), VFS_OK);
    CHECK_EQ(st.size, 21);
    size_t len;
    CHECK_EQ(vfs_read_file("/home/f.txt", buf, sizeof buf, &len), VFS_OK);
    CHECK_EQ(buf[15], 0);
    CHECK_EQ(buf[20], '!');
    CHECK_EQ(vfs_truncate("/home/f.txt", 3), VFS_OK);
    CHECK_EQ(vfs_stat("/home/f.txt", &st), VFS_OK);
    CHECK_EQ(st.size, 3);
    /* O_TRUNC */
    fd = vfs_open("/home/f.txt", VFS_O_WRITE | VFS_O_TRUNC);
    CHECK(fd >= 0);
    vfs_close(fd);
    CHECK_EQ(vfs_stat("/home/f.txt", &st), VFS_OK);
    CHECK_EQ(st.size, 0);
    /* write-only handle cannot read */
    fd = vfs_open("/home/f.txt", VFS_O_WRITE);
    CHECK_EQ(vfs_read(fd, buf, 1), VFS_EBADF);
    vfs_close(fd);
}

TEST(vfs_limits)
{
    fresh_vfs();
    /* open-file table exhaustion */
    int fds[VFS_MAX_OPEN];
    for (int i = 0; i < VFS_MAX_OPEN; i++)
        fds[i] = vfs_open("/system/welcome.txt", VFS_O_READ);
    CHECK(fds[VFS_MAX_OPEN - 1] >= 0);
    CHECK_EQ(vfs_open("/system/welcome.txt", VFS_O_READ), VFS_EMFILE);
    for (int i = 0; i < VFS_MAX_OPEN; i++)
        vfs_close(fds[i]);
    /* per-file limit; failed replace keeps the old content */
    static char big[RAMFS_MAX_FILE + 2];
    memset(big, 'x', sizeof big);
    CHECK_EQ(vfs_write_file("/home/a.txt", "keep", 4), VFS_OK);
    CHECK_EQ(vfs_write_file("/home/a.txt", big, RAMFS_MAX_FILE + 1), VFS_EFBIG);
    char buf[8];
    size_t len;
    CHECK_EQ(vfs_read_file("/home/a.txt", buf, sizeof buf, &len), VFS_OK);
    CHECK_EQ(len, 4);
    CHECK(memcmp(buf, "keep", 4) == 0);
    CHECK_EQ(vfs_write_file("/home/max.txt", big, RAMFS_MAX_FILE), VFS_OK);
    /* total capacity: fill until ENOSPC, never corrupting earlier files */
    int ok = 0, r = VFS_OK;
    char name[40];
    for (int i = 0; i < 100 && r == VFS_OK; i++) {
        snprintf(name, sizeof name, "/home/fill%d.bin", i);
        r = vfs_write_file(name, big, RAMFS_MAX_FILE);
        if (r == VFS_OK)
            ok++;
    }
    CHECK_EQ(r, VFS_ENOSPC);
    CHECK(ok >= 25 && ok <= 32);
    CHECK_EQ(vfs_read_file("/home/a.txt", buf, sizeof buf, &len), VFS_OK);
    CHECK(memcmp(buf, "keep", 4) == 0);
    struct vfs_usage u;
    vfs_usage(&u);
    CHECK(u.ram_bytes <= RAMFS_MAX_TOTAL);
}

TEST(vfs_listing_sorted_and_generation)
{
    fresh_vfs();
    uint32_t g0 = vfs_generation();
    CHECK_EQ(vfs_write_file("/home/notes/zeta.txt", "z", 1), VFS_OK);
    CHECK_EQ(vfs_write_file("/home/notes/alpha.txt", "a", 1), VFS_OK);
    CHECK_EQ(vfs_mkdir("/home/notes/mid"), VFS_OK);
    CHECK(vfs_generation() != g0);
    struct vfs_dirent d;
    const char *expect[] = { "alpha.txt", "mid", "seed.txt", "zeta.txt" };
    for (uint32_t i = 0; i < 4; i++) {
        CHECK_EQ(vfs_readdir("/home/notes", i, &d), VFS_OK);
        CHECK_STR(d.name, expect[i]);
    }
    CHECK_EQ(vfs_readdir("/home/notes", 4, &d), VFS_ENOENT);
    CHECK_EQ(vfs_readdir("/home/notes", 1, &d), VFS_OK);
    CHECK_EQ(d.type, VFS_DIR);
    /* multi-line content with punctuation round-trips byte for byte */
    const char *text = "Line one, with spaces.\nLine two: \"quotes\" & symbols!? (ok)\n\nend";
    CHECK_EQ(vfs_write_file("/home/notes/multi.txt", text, strlen(text)), VFS_OK);
    char buf[128];
    size_t len;
    CHECK_EQ(vfs_read_file("/home/notes/multi.txt", buf, sizeof buf, &len), VFS_OK);
    CHECK_EQ(len, strlen(text));
    CHECK(memcmp(buf, text, len) == 0);
}

void run_tests(void)
{
    RUN(normalize_rules);
    RUN(initramfs_validation);
    RUN(vfs_mounts_and_readonly);
    RUN(vfs_errors);
    RUN(vfs_read_write_offsets);
    RUN(vfs_limits);
    RUN(vfs_listing_sorted_and_generation);
}
