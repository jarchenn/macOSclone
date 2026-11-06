/* Persistent filesystem: round trips, limits, corruption handling and
 * simulated power loss after every single block write of a commit. */
#include "harness.h"
#include "fs/pfs.h"
#include "lib.h"
#include <stdlib.h>

const char *t_suite = "pfs";

#define NB 256

struct memdev {
    uint8_t blocks[NB][PFS_BLOCK];
    long writes;
    long fail_after; /* writes allowed before "power loss"; -1 = never */
    bool torn;       /* the failing write lands half-written */
};

static struct memdev dev_a, dev_b;

static int md_read(void *ctx, uint64_t b, void *buf)
{
    struct memdev *m = ctx;
    if (b >= NB)
        return PFS_E_IO;
    memcpy(buf, m->blocks[b], PFS_BLOCK);
    return 0;
}

static int md_write(void *ctx, uint64_t b, const void *buf)
{
    struct memdev *m = ctx;
    if (b >= NB)
        return PFS_E_IO;
    if (m->fail_after >= 0 && m->writes >= m->fail_after) {
        if (m->torn && m->writes == m->fail_after)
            memcpy(m->blocks[b], buf, PFS_BLOCK / 2); /* half a block reached the disk */
        m->writes++;
        return PFS_E_IO; /* power is gone: nothing else is written */
    }
    memcpy(m->blocks[b], buf, PFS_BLOCK);
    m->writes++;
    return 0;
}

static int md_flush(void *ctx)
{
    struct memdev *m = ctx;
    return (m->fail_after >= 0 && m->writes >= m->fail_after) ? PFS_E_IO : 0;
}

static struct pfs_blockdev mkdev(struct memdev *m)
{
    struct pfs_blockdev d = { m, NB, md_read, md_write, md_flush };
    return d;
}

/* Mount collector */
struct seen {
    int n;
    char path[64][PFS_MAX_PATH + 1];
    uint8_t type[64];
    uint32_t size[64];
    uint32_t crc[64];
};

static int collect(void *ctx, const struct pfs_entry *e, const uint8_t *data)
{
    struct seen *s = ctx;
    if (s->n >= 64)
        return -1;
    str_copy(s->path[s->n], e->path, sizeof s->path[0]);
    s->type[s->n] = e->type;
    s->size[s->n] = e->size;
    s->crc[s->n] = data ? crc32(data, e->size) : 0;
    s->n++;
    return 0;
}

static int find(const struct seen *s, const char *path)
{
    for (int i = 0; i < s->n; i++)
        if (!strcmp(s->path[i], path))
            return i;
    return -1;
}

static struct pfs_entry ent(const char *path, uint8_t type, const char *data)
{
    struct pfs_entry e;
    memset(&e, 0, sizeof e);
    str_copy(e.path, path, sizeof e.path);
    e.type = type;
    e.data = (const uint8_t *)data;
    e.size = data ? (uint32_t)strlen(data) : 0;
    e.dirty = type == 1;
    return e;
}

TEST(format_commit_remount)
{
    memset(&dev_a, 0, sizeof dev_a);
    dev_a.fail_after = -1;
    struct pfs_blockdev d = mkdev(&dev_a);
    struct pfs fs;
    bool formatted;
    CHECK_EQ(pfs_mount(&fs, &d, NULL, NULL, &formatted), PFS_OK);
    CHECK(formatted);
    static char big[PFS_MAX_FILE];
    for (size_t i = 0; i < sizeof big; i++)
        big[i] = (char)('a' + i % 23);
    big[sizeof big - 1] = 0;
    struct pfs_entry e[4] = { ent("notes", 2, NULL), ent("notes/a.txt", 1, "hello disk"),
                              ent("notes/empty.txt", 1, ""), ent("big.bin", 1, big) };
    CHECK_EQ(pfs_commit(&fs, e, 4), PFS_OK);
    CHECK(!e[1].dirty);
    CHECK_EQ(fs.generation, 2);
    pfs_unmount(&fs);
    struct seen s = { 0 };
    CHECK_EQ(pfs_mount(&fs, &d, collect, &s, &formatted), PFS_OK);
    CHECK(!formatted);
    CHECK_EQ(s.n, 4);
    int i = find(&s, "notes/a.txt");
    CHECK(i >= 0);
    if (i >= 0) {
        CHECK_EQ(s.size[i], 10);
        CHECK_EQ(s.crc[i], crc32("hello disk", 10));
    }
    CHECK(find(&s, "notes") >= 0 && s.type[find(&s, "notes")] == 2);
    CHECK_EQ(s.size[find(&s, "big.bin")], PFS_MAX_FILE - 1);
    CHECK_EQ(s.size[find(&s, "notes/empty.txt")], 0);
    pfs_unmount(&fs);
}

/* State 1 -> state 2, with power lost after every possible number of writes. */
TEST(power_loss_at_every_write)
{
    memset(&dev_a, 0, sizeof dev_a);
    dev_a.fail_after = -1;
    struct pfs_blockdev d = mkdev(&dev_a);
    struct pfs fs;
    CHECK_EQ(pfs_mount(&fs, &d, NULL, NULL, NULL), PFS_OK);
    struct pfs_entry s1[3] = { ent("notes", 2, NULL), ent("notes/a.txt", 1, "version one of a"), ent("notes/b.txt", 1, "bbbb") };
    CHECK_EQ(pfs_commit(&fs, s1, 3), PFS_OK);
    pfs_unmount(&fs);
    memcpy(&dev_b, &dev_a, sizeof dev_a); /* disk image holding state 1 */

    /* State 2: a.txt rewritten, b.txt deleted, c.txt added. */
    long total_writes = 0;
    {
        memcpy(&dev_a, &dev_b, sizeof dev_a);
        dev_a.writes = 0;
        CHECK_EQ(pfs_mount(&fs, &d, NULL, NULL, NULL), PFS_OK);
        struct pfs_entry s2[3] = { ent("notes", 2, NULL), ent("notes/a.txt", 1, "VERSION TWO of a, longer"), ent("notes/c.txt", 1, "new file c") };
        s2[0].dirty = false;
        CHECK_EQ(pfs_commit(&fs, s2, 3), PFS_OK);
        total_writes = dev_a.writes;
        pfs_unmount(&fs);
    }
    CHECK(total_writes >= 4);
    int old_states = 0, new_states = 0;
    for (int torn = 0; torn < 2; torn++) {
        for (long k = 0; k <= total_writes; k++) {
            memcpy(&dev_a, &dev_b, sizeof dev_a);
            dev_a.writes = 0;
            dev_a.fail_after = -1;
            CHECK_EQ(pfs_mount(&fs, &d, NULL, NULL, NULL), PFS_OK);
            dev_a.fail_after = k;
            dev_a.torn = torn;
            struct pfs_entry s2[3] = { ent("notes", 2, NULL), ent("notes/a.txt", 1, "VERSION TWO of a, longer"), ent("notes/c.txt", 1, "new file c") };
            int r = pfs_commit(&fs, s2, 3);
            pfs_unmount(&fs);
            /* "Reboot": power back, mount whatever is on the disk. */
            dev_a.fail_after = -1;
            struct seen s = { 0 };
            CHECK_EQ(pfs_mount(&fs, &d, collect, &s, NULL), PFS_OK);
            pfs_unmount(&fs);
            int a = find(&s, "notes/a.txt"), b = find(&s, "notes/b.txt"), c = find(&s, "notes/c.txt");
            bool is_old = s.n == 3 && a >= 0 && b >= 0 && c < 0 && s.crc[a] == crc32("version one of a", 16);
            bool is_new = s.n == 3 && a >= 0 && b < 0 && c >= 0 && s.crc[a] == crc32("VERSION TWO of a, longer", 24);
            if (!(is_old || is_new)) {
                CHECK(is_old || is_new);
                fprintf(stderr, "    crash after %ld writes (torn=%d) gave a mixed state\n", k, torn);
            }
            if (r == PFS_OK)
                CHECK(is_new); /* a reported success is durable */
            old_states += is_old;
            new_states += is_new;
        }
    }
    CHECK(old_states > 0 && new_states > 0);
}

TEST(corruption_detected)
{
    memset(&dev_a, 0, sizeof dev_a);
    dev_a.fail_after = -1;
    struct pfs_blockdev d = mkdev(&dev_a);
    struct pfs fs;
    CHECK_EQ(pfs_mount(&fs, &d, NULL, NULL, NULL), PFS_OK);
    struct pfs_entry e[2] = { ent("x.txt", 1, "file x content"), ent("y.txt", 1, "file y content") };
    CHECK_EQ(pfs_commit(&fs, e, 2), PFS_OK);
    uint32_t xblock = e[0].start;
    pfs_unmount(&fs);
    /* Flip a byte inside x.txt's data: x is dropped, y survives. */
    dev_a.blocks[xblock][3] ^= 0x40;
    struct seen s = { 0 };
    CHECK_EQ(pfs_mount(&fs, &d, collect, &s, NULL), PFS_OK);
    CHECK(find(&s, "x.txt") < 0);
    CHECK(find(&s, "y.txt") >= 0);
    CHECK_EQ(fs.stats.corrupt_files, 1);
    pfs_unmount(&fs);
    /* Damage the newest superblock: the older generation is used instead. */
    memset(&dev_a, 0, sizeof dev_a);
    dev_a.fail_after = -1;
    CHECK_EQ(pfs_mount(&fs, &d, NULL, NULL, NULL), PFS_OK);
    struct pfs_entry g2[1] = { ent("gen.txt", 1, "two") };
    CHECK_EQ(pfs_commit(&fs, g2, 1), PFS_OK);
    struct pfs_entry g3[1] = { ent("gen.txt", 1, "three") };
    CHECK_EQ(pfs_commit(&fs, g3, 1), PFS_OK);
    int newest = fs.slot;
    pfs_unmount(&fs);
    dev_a.blocks[newest][20] ^= 1;
    memset(&s, 0, sizeof s);
    CHECK_EQ(pfs_mount(&fs, &d, collect, &s, NULL), PFS_OK);
    CHECK(find(&s, "gen.txt") >= 0 && s.size[find(&s, "gen.txt")] == 3);
    pfs_unmount(&fs);
    /* Both superblocks damaged but recognisable: refuse, never reformat. */
    dev_a.blocks[0][20] ^= 1;
    dev_a.blocks[1][20] ^= 1;
    if (newest == 0)
        dev_a.blocks[0][20] ^= 1, dev_a.blocks[0][21] ^= 1;
    bool formatted = true;
    CHECK_EQ(pfs_mount(&fs, &d, NULL, NULL, &formatted), PFS_E_CORRUPT);
    CHECK(!formatted);
}

TEST(space_limits)
{
    memset(&dev_a, 0, sizeof dev_a);
    dev_a.fail_after = -1;
    struct pfs_blockdev d = mkdev(&dev_a);
    struct pfs fs;
    CHECK_EQ(pfs_mount(&fs, &d, NULL, NULL, NULL), PFS_OK);
    static char big[PFS_MAX_FILE];
    memset(big, 'z', sizeof big - 1);
    big[sizeof big - 1] = 0;
    static struct pfs_entry e[16];
    static char names[16][16];
    for (int i = 0; i < 16; i++) {
        snprintf(names[i], sizeof names[i], "f%d", i);
        e[i] = ent(names[i], 1, big);
    }
    /* 16 files x 16 blocks + catalog do not fit in 254 data blocks. */
    CHECK_EQ(pfs_commit(&fs, e, 16), PFS_E_NOSPC);
    CHECK_EQ(fs.generation, 1); /* nothing committed */
    CHECK_EQ(pfs_commit(&fs, e, 6), PFS_OK);
    pfs_unmount(&fs);
    struct seen s = { 0 };
    CHECK_EQ(pfs_mount(&fs, &d, collect, &s, NULL), PFS_OK);
    CHECK_EQ(s.n, 6);
    /* Rewriting all 6 needs fresh blocks while the old copies stay live. */
    for (int i = 0; i < 6; i++)
        e[i].dirty = true;
    CHECK_EQ(pfs_commit(&fs, e, 6), PFS_OK);
    pfs_unmount(&fs);
    struct pfs_blockdev tiny = { &dev_a, 16, md_read, md_write, md_flush };
    CHECK_EQ(pfs_mount(&fs, &tiny, NULL, NULL, NULL), PFS_E_SMALL);
}

void run_tests(void)
{
    RUN(format_commit_remount);
    RUN(power_loss_at_every_write);
    RUN(corruption_detected);
    RUN(space_limits);
}
