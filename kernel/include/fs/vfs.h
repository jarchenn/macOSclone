#ifndef AURORA_VFS_H
#define AURORA_VFS_H
#include "types.h"

/* Aurora VFS. One namespace rooted at "/":
 *   /system  read-only initramfs (boot module), data stays in module memory
 *   /home    writable RAM filesystem (session only: lost on restart)
 * Paths are absolute after normalisation, at most VFS_PATH_MAX bytes, with
 * components of 1..VFS_NAME_MAX characters from [A-Za-z0-9._-]. */

#define VFS_NAME_MAX 63
#define VFS_PATH_MAX 255
#define VFS_MAX_OPEN 32

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
};

#define VFS_O_READ 1u
#define VFS_O_WRITE 2u
#define VFS_O_CREATE 4u
#define VFS_O_TRUNC 8u

struct vfs_stat {
    uint8_t type;
    bool readonly;
    uint32_t size;
};

struct vfs_dirent {
    char name[VFS_NAME_MAX + 1];
    uint8_t type;
    bool readonly;
    uint32_t size;
};

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
/* index-th entry (sorted by name) of a directory; ENOENT past the end. */
int vfs_readdir(const char *path, uint32_t index, struct vfs_dirent *out);
/* Whole-file helpers. vfs_write_file replaces content atomically: on any
 * error the previous content is unchanged. */
int vfs_read_file(const char *path, void *buf, size_t cap, size_t *len);
int vfs_write_file(const char *path, const void *data, size_t len);

uint32_t vfs_generation(void); /* increments on every mutation */
void vfs_usage(struct vfs_usage *u);

#endif
