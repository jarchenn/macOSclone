/* VFS remove/rename and the persistent /home on a simulated disk. */
#include "harness.h"
#include "fs/vfs.h"
#include "fs/pfs.h"
#include "fs/initramfs.h"
#include "lib.h"
#include <stdlib.h>

const char *t_suite = "vfs_disk";

#define NB 1024
static uint8_t disk[NB][PFS_BLOCK];
static int rd(void *c, uint64_t b, void *buf) { (void)c; if (b >= NB) return PFS_E_IO; memcpy(buf, disk[b], PFS_BLOCK); return 0; }
static int wr(void *c, uint64_t b, const void *buf) { (void)c; if (b >= NB) return PFS_E_IO; memcpy(disk[b], buf, PFS_BLOCK); return 0; }
static int fl(void *c) { (void)c; return 0; }
static struct pfs_blockdev dev = { NULL, NB, rd, wr, fl };

/* minimal archive: home/notes/seed.txt */
static uint8_t archive[512];
static size_t make_archive(void)
{
    const char *paths[] = { "home", "home/notes", "home/notes/seed.txt" };
    uint32_t types[] = { 1, 1, 2 };
    const char *data = "seed";
    size_t off = 32;
    memset(archive, 0, sizeof archive);
    for (int i = 0; i < 3; i++) {
        uint32_t pl = (uint32_t)strlen(paths[i]);
        uint64_t dl = types[i] == 2 ? 4 : 0;
        memcpy(archive + off, &types[i], 4);
        memcpy(archive + off + 4, &pl, 4);
        memcpy(archive + off + 8, &dl, 8);
        off += 16;
        memcpy(archive + off, paths[i], pl);
        off += ALIGN_UP(pl, 8);
        if (dl) {
            memcpy(archive + off, data, dl);
            off += ALIGN_UP(dl, 8);
        }
    }
    memcpy(archive, "AURFS01\0", 8);
    uint32_t v = 1, n = 3, crc = crc32(archive + 32, off - 32);
    uint64_t total = off;
    memcpy(archive + 8, &v, 4);
    memcpy(archive + 12, &n, 4);
    memcpy(archive + 16, &total, 8);
    memcpy(archive + 24, &crc, 4);
    return off;
}

static void boot(bool *formatted)
{
    vfs_init();
    CHECK_EQ(vfs_load_initramfs(archive, make_archive()), VFS_OK);
    CHECK_EQ(vfs_attach_persistent(&dev, formatted), VFS_OK);
    CHECK(vfs_home_persistent());
}

TEST(remove_and_rename_in_ram)
{
    vfs_init();
    CHECK_EQ(vfs_mkdir("/home/d"), VFS_OK);
    CHECK_EQ(vfs_write_file("/home/d/a.txt", "A", 1), VFS_OK);
    CHECK_EQ(vfs_remove("/home/d"), VFS_ENOTEMPTY);
    int fd = vfs_open("/home/d/a.txt", VFS_O_READ);
    CHECK_EQ(vfs_remove("/home/d/a.txt"), VFS_EBUSY);
    vfs_close(fd);
    CHECK_EQ(vfs_rename("/home/d/a.txt", "/home/b.txt"), VFS_OK);
    struct vfs_stat st;
    CHECK_EQ(vfs_stat("/home/d/a.txt", &st), VFS_ENOENT);
    CHECK_EQ(vfs_stat("/home/b.txt", &st), VFS_OK);
    CHECK_EQ(vfs_rename("/home/d", "/home/d/inside"), VFS_EINVAL); /* into itself */
    CHECK_EQ(vfs_write_file("/home/c.txt", "C", 1), VFS_OK);
    CHECK_EQ(vfs_rename("/home/b.txt", "/home/c.txt"), VFS_EEXIST);
    CHECK_EQ(vfs_rename("/home/b.txt", "/system/x"), VFS_ENOENT);  /* no /system here */
    CHECK_EQ(vfs_remove("/home/d"), VFS_OK);
    CHECK_EQ(vfs_remove("/home"), VFS_EINVAL);   /* mount point */
    CHECK_EQ(vfs_remove("/home/nope"), VFS_ENOENT);
    struct vfs_usage u1, u2;
    vfs_usage(&u1);
    CHECK_EQ(vfs_write_file("/home/big.bin", "0123456789", 10), VFS_OK);
    CHECK_EQ(vfs_remove("/home/big.bin"), VFS_OK);
    vfs_usage(&u2);
    CHECK_EQ(u1.ram_bytes, u2.ram_bytes); /* quota released */
    CHECK_EQ(u1.ram_nodes, u2.ram_nodes);
}

TEST(persistent_home_round_trip)
{
    memset(disk, 0, sizeof disk);
    bool formatted = false;
    boot(&formatted);
    CHECK(formatted);
    struct vfs_stat st;
    CHECK_EQ(vfs_stat("/home/notes/seed.txt", &st), VFS_OK); /* seed kept on a new disk */
    CHECK_EQ(vfs_write_file("/home/notes/a.txt", "persist me", 10), VFS_OK);
    CHECK_EQ(vfs_mkdir("/home/proj"), VFS_OK);
    int fd = vfs_open("/home/proj/log.txt", VFS_O_WRITE | VFS_O_CREATE);
    CHECK(fd >= 0);
    CHECK_EQ(vfs_write(fd, "line1\n", 6), 6);
    CHECK_EQ(vfs_write(fd, "line2\n", 6), 6);
    CHECK_EQ(vfs_close(fd), VFS_OK); /* commit happens here */
    CHECK_EQ(vfs_remove("/home/notes/seed.txt"), VFS_OK);
    CHECK_EQ(vfs_rename("/home/notes/a.txt", "/home/a.txt"), VFS_OK);
    struct vfs_storage s;
    vfs_storage(&s);
    CHECK(s.persistent && s.commits >= 5 && s.last_error == 0);

    /* "Reboot": a fresh VFS sees the disk, not the initramfs seed. */
    boot(&formatted);
    CHECK(!formatted);
    char buf[32];
    size_t len;
    CHECK_EQ(vfs_read_file("/home/a.txt", buf, sizeof buf, &len), VFS_OK);
    CHECK_EQ(len, 10);
    CHECK(memcmp(buf, "persist me", 10) == 0);
    CHECK_EQ(vfs_read_file("/home/proj/log.txt", buf, sizeof buf, &len), VFS_OK);
    CHECK_EQ(len, 12);
    CHECK_EQ(vfs_stat("/home/notes/seed.txt", &st), VFS_ENOENT); /* deletion persisted */
    CHECK_EQ(vfs_stat("/home/notes/a.txt", &st), VFS_ENOENT);    /* rename persisted */
    CHECK_EQ(vfs_stat("/home/notes", &st), VFS_OK);
}

TEST(disk_full_reported)
{
    memset(disk, 0, sizeof disk);
    boot(NULL);
    static char big[RAMFS_MAX_FILE];
    memset(big, 'q', sizeof big);
    /* 1024 blocks: ~30 x 64 KiB files fit within RAM quota (2 MiB) and disk. */
    int r = VFS_OK, n = 0;
    char name[32];
    for (int i = 0; i < 40 && r == VFS_OK; i++) {
        snprintf(name, sizeof name, "/home/f%d", i);
        r = vfs_write_file(name, big, sizeof big);
        n += r == VFS_OK;
    }
    CHECK(r == VFS_ENOSPC);
    CHECK(n >= 20);
}

void run_tests(void)
{
    RUN(remove_and_rename_in_ram);
    RUN(persistent_home_round_trip);
    RUN(disk_full_reported);
}
