/* The 0.1 apps were written against the kernel's VFS (fs/vfs.h). As user
 * processes (0.7) they get the same calls implemented over POSIX here, with
 * errno mapped back to the VFS codes so their messages stay the same.
 * vfs_normalize/vfs_split come from kernel/fs/path.c (portable). */
#include "fs/vfs.h"
#include "lib.h"
#include <aurora.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int verr(int e)
{
    switch (e) {
    case 0: return VFS_OK;
    case ENOENT: return VFS_ENOENT;
    case EEXIST: return VFS_EEXIST;
    case ENOTDIR: return VFS_ENOTDIR;
    case EISDIR: return VFS_EISDIR;
    case EROFS: return VFS_EROFS;
    case ENOSPC: return VFS_ENOSPC;
    case EINVAL: return VFS_EINVAL;
    case EBADF: return VFS_EBADF;
    case ENAMETOOLONG: return VFS_ENAMETOOLONG;
    case EMFILE: case ENFILE: return VFS_EMFILE;
    case ENOMEM: return VFS_ENOMEM;
    case EFBIG: return VFS_EFBIG;
    case EBUSY: return VFS_EBUSY;
    case ENOTEMPTY: return VFS_ENOTEMPTY;
    case EXDEV: return VFS_EXDEV;
    case EACCES: case EPERM: return VFS_EROFS; /* closest 0.1 message for /system */
    default: return VFS_EIO;
    }
}

const char *vfs_strerror(int err)
{
    switch (err) {
    case VFS_OK: return "success";
    case VFS_ENOENT: return "no such file or directory";
    case VFS_EEXIST: return "file already exists";
    case VFS_ENOTDIR: return "not a directory";
    case VFS_EISDIR: return "is a directory";
    case VFS_EROFS: return "read-only file system";
    case VFS_ENOSPC: return "RAM storage is full";
    case VFS_EINVAL: return "invalid path or argument";
    case VFS_EBADF: return "bad file handle";
    case VFS_ENAMETOOLONG: return "name too long";
    case VFS_EMFILE: return "too many open files";
    case VFS_ENOMEM: return "out of memory";
    case VFS_EFBIG: return "file too large (64 KiB limit)";
    case VFS_EBADARCHIVE: return "corrupt initramfs archive";
    case VFS_EBUSY: return "file is in use";
    case VFS_ENOTEMPTY: return "directory not empty";
    case VFS_EIO: return "disk I/O error";
    case VFS_EXDEV: return "cannot move between file systems";
    default: return "unknown error";
    }
}

int vfs_stat(const char *path, struct vfs_stat *st)
{
    struct stat s;
    if (stat(path, &s) != 0)
        return verr(errno);
    memset(st, 0, sizeof *st);
    st->type = S_ISDIR(s.st_mode) ? VFS_DIR : VFS_FILE;
    st->size = (uint32_t)s.st_size;
    st->mode = (uint16_t)(s.st_mode & 07777);
    st->uid = s.st_uid;
    st->gid = s.st_gid;
    st->ino = (uint32_t)s.st_ino;
    st->mtime = (int64_t)s.st_mtime;
    /* /system is the read-only initramfs; everything else is writable for
     * the (root) desktop session. */
    st->readonly = !strcmp(path, "/system") || !strncmp(path, "/system/", 8) || !strcmp(path, "/") ||
                   !strcmp(path, "/dev") || !strncmp(path, "/dev/", 5);
    return VFS_OK;
}

static int cmp_names(const void *a, const void *b)
{
    return strcmp(((const struct vfs_dirent *)a)->name, ((const struct vfs_dirent *)b)->name);
}

/* The kernel VFS returned entries sorted by name; keep a sorted cache of
 * the last directory listed so index-based iteration stays cheap. */
static char cache_path[VFS_PATH_MAX + 1];
static struct vfs_dirent *cache;
static uint32_t cache_n;
static uint64_t cache_gen = ~0ull;

int vfs_readdir(const char *path, uint32_t index, struct vfs_dirent *out)
{
    uint64_t gen = vfs_generation();
    if (index == 0 || strcmp(path, cache_path) || gen != cache_gen) {
        free(cache);
        cache = NULL;
        cache_n = 0;
        str_copy(cache_path, path, sizeof cache_path);
        cache_gen = gen;
        DIR *d = opendir(path);
        if (!d)
            return verr(errno);
        uint32_t cap = 0;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            if (cache_n == cap) {
                cap = cap ? cap * 2 : 32;
                struct vfs_dirent *n = realloc(cache, cap * sizeof *n);
                if (!n)
                    break;
                cache = n;
            }
            struct vfs_dirent *v = &cache[cache_n];
            memset(v, 0, sizeof *v);
            str_copy(v->name, e->d_name, sizeof v->name);
            char full[VFS_PATH_MAX + 2];
            snprintf(full, sizeof full, "%s/%s", strcmp(path, "/") ? path : "", e->d_name);
            struct vfs_stat st;
            if (vfs_stat(full, &st) == VFS_OK) {
                v->type = st.type;
                v->size = st.size;
                v->readonly = st.readonly;
                v->ino = st.ino;
            } else {
                v->type = VFS_FILE;
            }
            cache_n++;
        }
        closedir(d);
        qsort(cache, cache_n, sizeof *cache, cmp_names);
    }
    if (index >= cache_n)
        return VFS_ENOENT;
    *out = cache[index];
    return VFS_OK;
}

int vfs_read_file(const char *path, void *buf, size_t cap, size_t *len)
{
    *len = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return verr(errno);
    struct stat s;
    if (fstat(fd, &s) == 0 && S_ISDIR(s.st_mode)) {
        close(fd);
        return VFS_EISDIR;
    }
    size_t n = 0;
    for (;;) {
        if (n == cap) {
            char c;
            ssize_t more = read(fd, &c, 1);
            close(fd);
            *len = n;
            return more > 0 ? VFS_EFBIG : VFS_OK;
        }
        ssize_t r = read(fd, (char *)buf + n, cap - n);
        if (r < 0) {
            int e = errno;
            close(fd);
            return verr(e);
        }
        if (r == 0)
            break;
        n += (size_t)r;
    }
    close(fd);
    *len = n;
    return VFS_OK;
}

/* Atomic replace: write a temporary file next to the target, then rename
 * it over the target (0.6 rename replaces in one commit). */
int vfs_write_file(const char *path, const void *data, size_t len)
{
    char tmp[VFS_PATH_MAX + 1];
    char parent[VFS_PATH_MAX + 1], name[VFS_NAME_MAX + 1];
    int r = vfs_split(path, parent, sizeof parent, name, sizeof name);
    if (r)
        return r;
    struct stat ps;
    if (stat(parent, &ps) != 0)
        return verr(errno);
    struct stat ts;
    if (stat(path, &ts) == 0 && S_ISDIR(ts.st_mode))
        return VFS_EISDIR;
    snprintf(tmp, sizeof tmp, "%s/.%.40s.%d.tmp", strcmp(parent, "/") ? parent : "", name, (int)getpid());
    if (strlen(tmp) > VFS_PATH_MAX)
        return VFS_ENAMETOOLONG;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return verr(errno);
    size_t done = 0;
    while (done < len) {
        ssize_t w = write(fd, (const char *)data + done, len - done);
        if (w < 0) {
            int e = errno;
            close(fd);
            unlink(tmp);
            return verr(e);
        }
        done += (size_t)w;
    }
    if (close(fd) != 0 || rename(tmp, path) != 0) {
        int e = errno;
        unlink(tmp);
        return verr(e);
    }
    return VFS_OK;
}

int vfs_mkdir(const char *path) { return mkdir(path, 0755) == 0 ? VFS_OK : verr(errno); }

int vfs_create(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0)
        return verr(errno);
    close(fd);
    return VFS_OK;
}

int vfs_remove(const char *path)
{
    struct stat s;
    if (stat(path, &s) != 0)
        return verr(errno);
    int r = S_ISDIR(s.st_mode) ? rmdir(path) : unlink(path);
    return r == 0 ? VFS_OK : verr(errno);
}

int vfs_rename(const char *from, const char *to) { return rename(from, to) == 0 ? VFS_OK : verr(errno); }

int vfs_sync(void)
{
    sync();
    return VFS_OK;
}

static struct aurora_klog klog_now(void)
{
    struct aurora_klog k;
    memset(&k, 0, sizeof k);
    aurora_klog(&k);
    return k;
}

uint32_t vfs_generation(void) { return (uint32_t)klog_now().vfs_generation; }
bool vfs_home_persistent(void) { return klog_now().home_persistent != 0; }

void vfs_storage(struct vfs_storage *out)
{
    struct aurora_klog k = klog_now();
    memset(out, 0, sizeof *out);
    out->persistent = k.home_persistent;
    out->generation = k.vfs_generation;
    out->commits = k.pfs_commits;
    out->blocks_written = k.pfs_blocks_written;
    out->used_blocks = k.pfs_used_blocks;
    out->total_blocks = k.pfs_total_blocks;
    out->corrupt_files = k.pfs_corrupt_files;
    out->last_error = k.pfs_last_error;
}

void vfs_usage(struct vfs_usage *u)
{
    struct aurora_klog k = klog_now();
    u->ram_bytes = k.ram_bytes;
    u->ram_nodes = k.ram_nodes;
    u->open_files = k.open_files;
}
