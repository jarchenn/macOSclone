#ifndef AURORA_VFS_H
#define AURORA_VFS_H
#include "types.h"

/* Aurora VFS. One namespace rooted at "/":
 *   /system  read-only initramfs (boot module), data stays in module memory
 *   /home    writable RAM filesystem, or PFS on the data disk (0.3)
 *   /tmp     writable RAM filesystem (always lost on restart)
 * Every node has a mode (permission bits), an owner and a group (0.6). The
 * VFS stores them; the system call layer enforces them.
 * Paths are absolute after normalisation, at most VFS_PATH_MAX bytes, with
 * components of 1..VFS_NAME_MAX characters from [A-Za-z0-9._-]. */

#define VFS_NAME_MAX 63
#define VFS_PATH_MAX 255
#define VFS_MAX_OPEN 256

/* RAM filesystem limits */
#define RAMFS_MAX_FILE (64u * 1024u)
#define RAMFS_MAX_TOTAL (2u * 1024u * 1024u)
#define RAMFS_MAX_NODES 512u

enum vfs_type { VFS_FILE = 1, VFS_DIR = 2 };

enum vfs_err {
    VFS_OK = 0,
    VFS_ENOENT = -1,
    VFS_EEXIST = -2,
    VFS_ENOTDIR = -3,
    VFS_EISDIR = -4,
    VFS_EROFS = -5,
    VFS_ENOSPC = -6,
    VFS_EINVAL = -7,
    VFS_EBADF = -8,
    VFS_ENAMETOOLONG = -9,
    VFS_EMFILE = -10,
    VFS_ENOMEM = -11,
    VFS_EFBIG = -12,
    VFS_EBADARCHIVE = -13,
    VFS_EBUSY = -14,
    VFS_ENOTEMPTY = -15,
    VFS_EIO = -16,
    VFS_EXDEV = -17,
};

#define VFS_O_READ 1u
#define VFS_O_WRITE 2u
#define VFS_O_CREATE 4u
#define VFS_O_TRUNC 8u
#define VFS_O_EXCL 16u
#define VFS_O_APPEND 32u

struct vfs_stat {
    uint8_t type;
    bool readonly;
    uint32_t size;
    uint16_t mode; /* permission bits (07777) */
    uint32_t uid, gid;
    uint32_t ino;
    int64_t mtime;
};

struct vfs_dirent {
    char name[VFS_NAME_MAX + 1];
    uint8_t type;
    bool readonly;
    uint32_t size;
    uint32_t ino;
};

/* Ownership and mode for nodes created by the next calls (0.6). The
 * defaults (uid 0, gid 0, 0644 files, 0755 directories) apply to kernel
 * callers such as the desktop apps. */
struct vfs_cred {
    uint32_t uid, gid;
    uint16_t mode; /* already masked by the creator's umask */
};
int vfs_open_as(const char *path, unsigned flags, const struct vfs_cred *c);
int vfs_mkdir_as(const char *path, const struct vfs_cred *c);
int vfs_fstat(int fd, struct vfs_stat *st);
int vfs_ftruncate(int fd, uint32_t size);
int vfs_tell(int fd);
int vfs_chmod(const char *path, uint16_t mode);
int vfs_chown(const char *path, uint32_t uid, uint32_t gid);
/* Clock used for modification times (seconds since 1970); NULL = 0. */
void vfs_set_clock(int64_t (*now)(void));

struct vfs_usage {
    uint32_t ram_bytes, ram_nodes, open_files;
};

const char *vfs_strerror(int err);

/* Path helpers (fs/path.c) */
bool vfs_valid_name(const char *name, size_t len);
int vfs_normalize(const char *cwd, const char *path, char *out, size_t cap);
/* Splits a normalised path into parent directory and final component. */
int vfs_split(const char *norm, char *parent, size_t pcap, char *name, size_t ncap);

void vfs_init(void);
/* Mounts an AURFS archive: top-level "home/" entries are copied into the RAM
 * filesystem, everything else becomes read-only nodes backed by `data`. */
int vfs_load_initramfs(const uint8_t *data, size_t size);

int vfs_stat(const char *path, struct vfs_stat *st);
int vfs_open(const char *path, unsigned flags);
int vfs_close(int fd);
int vfs_read(int fd, void *buf, size_t n);   /* bytes read or error */
int vfs_write(int fd, const void *buf, size_t n);
int vfs_seek(int fd, uint32_t offset);
int vfs_mkdir(const char *path);
int vfs_create(const char *path);            /* empty file; EEXIST if present */
int vfs_truncate(const char *path, uint32_t size);
/* Removes a file or an empty directory (EBUSY if a file is open). */
int vfs_remove(const char *path);
/* Renames/moves within one filesystem (same backend). */
int vfs_rename(const char *from, const char *to);
/* Flushes persistent storage; returns 0 when everything is durable. */
int vfs_sync(void);
/* index-th entry (sorted by name) of a directory; ENOENT past the end. */
int vfs_readdir(const char *path, uint32_t index, struct vfs_dirent *out);
/* Whole-file helpers. vfs_write_file replaces content atomically: on any
 * error the previous content is unchanged. */
int vfs_read_file(const char *path, void *buf, size_t cap, size_t *len);
int vfs_write_file(const char *path, const void *data, size_t len);

uint32_t vfs_generation(void); /* increments on every mutation */

/* Persistent /home (0.3). When a data disk is attached, /home is stored on
 * it with the PFS format; every completed mutation is committed before the
 * call returns, so a successful save survives power loss. */
struct pfs_blockdev;
int vfs_attach_persistent(struct pfs_blockdev *dev, bool *formatted);
bool vfs_home_persistent(void);
struct vfs_storage {
    bool persistent;
    uint64_t generation, commits, blocks_written;
    uint32_t used_blocks, total_blocks, corrupt_files;
    int last_error;
};
void vfs_storage(struct vfs_storage *out);
void vfs_usage(struct vfs_usage *u);

#endif
