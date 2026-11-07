#ifndef AURORA_PFS_H
#define AURORA_PFS_H
#include "types.h"

/* Aurora persistent filesystem (PFS v1): shadow-paged ("copy-on-write")
 * on-disk format for the /home tree. Portable and host-tested.
 *
 * Disk layout (4 KiB blocks):
 *   block 0, block 1   superblock slots A and B (CRC-protected)
 *   blocks 2..N-1      data area: file extents and catalog extents
 *
 * The catalog lists every directory and file (relative path, type, size,
 * extent, CRC-32 of the content). A commit:
 *   1. writes changed files to blocks unused by the committed state,
 *   2. writes a new catalog the same way,
 *   3. flushes, then writes the superblock with generation+1 into the slot
 *      that does NOT hold the current superblock, and flushes again.
 * A crash before step 3 completes leaves the previous superblock, catalog and
 * extents untouched; a torn superblock fails its CRC and the other slot wins.
 * Mount picks the valid superblock with the highest generation. */

#define PFS_BLOCK 4096u
#define PFS_MAX_ENTRIES 512
#define PFS_MAX_PATH 255
#define PFS_MAX_FILE (64u * 1024u)
#define PFS_MIN_BLOCKS 64

enum pfs_err {
    PFS_OK = 0,
    PFS_E_IO = -1,
    PFS_E_NOSPC = -2,
    PFS_E_CORRUPT = -3,
    PFS_E_TOOMANY = -4,
    PFS_E_SMALL = -5,
    PFS_E_NOMEM = -6,
};

struct pfs_blockdev {
    void *ctx;
    uint64_t nblocks;
    int (*read)(void *ctx, uint64_t block, void *buf);        /* 0 or PFS_E_IO */
    int (*write)(void *ctx, uint64_t block, const void *buf); /* 0 or PFS_E_IO */
    int (*flush)(void *ctx);
};

struct pfs_entry {
    char path[PFS_MAX_PATH + 1]; /* relative to the mount point, no leading '/' */
    uint8_t type;                /* 1 = file, 2 = directory (VFS_FILE / VFS_DIR) */
    uint32_t size;
    const uint8_t *data;         /* content for dirty files (size bytes) */
    bool dirty;                  /* content changed since the last commit */
    uint32_t start, nblocks;     /* extent; updated by pfs_commit */
    uint32_t crc;
    /* Metadata (format v2; v1 catalogs load with mode 0644/0755, owner 0). */
    uint16_t mode;
    uint32_t uid, gid;
    int64_t mtime;
};

struct pfs_stats {
    uint64_t commits, blocks_written, mounts, corrupt_files;
    uint64_t generation;
    uint32_t used_blocks, total_blocks;
};

struct pfs {
    struct pfs_blockdev *dev;
    uint64_t generation;
    int slot;              /* slot holding the committed superblock */
    uint64_t nblocks;
    uint8_t *committed;    /* 1 bit per block: referenced by the committed state */
    uint8_t *pending;      /* scratch bitmap for the state being built */
    uint8_t *buf;          /* one block */
    struct pfs_stats stats;
};

typedef int (*pfs_emit_fn)(void *ctx, const struct pfs_entry *e, const uint8_t *data);

/* Mounts dev, formatting it first if it carries no valid superblock.
 * Calls emit() for every directory (parents first) and every file whose
 * content passed its CRC. Corrupt files are skipped and counted. */
int pfs_mount(struct pfs *fs, struct pfs_blockdev *dev, pfs_emit_fn emit, void *ctx, bool *formatted);
/* Commits the complete state described by entries[0..n). Clean files keep
 * their extents; dirty files are rewritten. On success the entries' extents
 * describe the new state; on failure the on-disk state is unchanged. */
int pfs_commit(struct pfs *fs, struct pfs_entry *entries, size_t n);
void pfs_unmount(struct pfs *fs);
const char *pfs_strerror(int err);

#endif
