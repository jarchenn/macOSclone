/* Writable RAM filesystem backend. Data lives in the kernel heap and is lost
 * on restart. Enforces per-file, total-byte and node-count limits. */
#include "fs/fs_internal.h"
#include "kalloc.h"
#include "lib.h"

static uint32_t bytes_used;
static uint32_t nodes_used;

void ramfs_reset_usage(void)
{
    bytes_used = 0;
    nodes_used = 0;
}
uint32_t ramfs_bytes_used(void) { return bytes_used; }
uint32_t ramfs_nodes_used(void) { return nodes_used; }

static int ram_create(struct vfs_node *parent, const char *name, uint8_t type, struct vfs_node **out)
{
    if (nodes_used >= RAMFS_MAX_NODES)
        return VFS_ENOSPC;
    struct vfs_node *n = vfs_node_new(name, type, &ramfs_backend);
    if (!n)
        return VFS_ENOMEM;
    nodes_used++;
    vfs_node_attach(parent, n);
    *out = n;
    return VFS_OK;
}

/* Ensures capacity >= need; new storage is allocated before the old buffer
 * is released so failure leaves the node unchanged. */
static int reserve(struct vfs_node *n, uint32_t need)
{
    if (need > RAMFS_MAX_FILE)
        return VFS_EFBIG;
    if (need <= n->capacity)
        return VFS_OK;
    uint32_t cap = n->capacity ? n->capacity : 256;
    while (cap < need)
        cap *= 2;
    if (cap > RAMFS_MAX_FILE)
        cap = RAMFS_MAX_FILE;
    if (bytes_used - n->capacity + cap > RAMFS_MAX_TOTAL)
        return VFS_ENOSPC;
    uint8_t *d = kmalloc(cap);
    if (!d)
        return VFS_ENOMEM;
    if (n->size)
        memcpy(d, n->data, n->size);
    if (n->data)
        kfree(n->data);
    bytes_used = bytes_used - n->capacity + cap;
    n->data = d;
    n->capacity = cap;
    return VFS_OK;
}

static int ram_write(struct vfs_node *n, uint32_t off, const void *buf, uint32_t len)
{
    if (len == 0)
        return VFS_OK;
    uint64_t end = (uint64_t)off + len;
    if (end > RAMFS_MAX_FILE)
        return VFS_EFBIG;
    int r = reserve(n, (uint32_t)end);
    if (r)
        return r;
    if (off > n->size)
        memset(n->data + n->size, 0, off - n->size); /* zero-fill a gap */
    memcpy(n->data + off, buf, len);
    if (end > n->size)
        n->size = (uint32_t)end;
    return VFS_OK;
}

static int ram_truncate(struct vfs_node *n, uint32_t size)
{
    if (size > n->size) {
        int r = reserve(n, size);
        if (r)
            return r;
        memset(n->data + n->size, 0, size - n->size);
    }
    n->size = size;
    return VFS_OK;
}

static int ram_replace(struct vfs_node *n, const void *buf, uint32_t len)
{
    if (len > RAMFS_MAX_FILE)
        return VFS_EFBIG;
    if (len == 0) {
        n->size = 0;
        return VFS_OK;
    }
    if (len <= n->capacity) {
        memmove(n->data, buf, len);
        n->size = len;
        return VFS_OK;
    }
    uint32_t cap = 256;
    while (cap < len)
        cap *= 2;
    if (cap > RAMFS_MAX_FILE)
        cap = RAMFS_MAX_FILE;
    if (bytes_used - n->capacity + cap > RAMFS_MAX_TOTAL)
        return VFS_ENOSPC;
    uint8_t *d = kmalloc(cap);
    if (!d)
        return VFS_ENOMEM;
    memcpy(d, buf, len);
    if (n->data)
        kfree(n->data);
    bytes_used = bytes_used - n->capacity + cap;
    n->data = d;
    n->capacity = cap;
    n->size = len;
    return VFS_OK;
}

static int ram_remove(struct vfs_node *n)
{
    bytes_used -= n->capacity;
    if (nodes_used)
        nodes_used--;
    return VFS_OK;
}

const struct fs_backend ramfs_backend = {
    .name = "ramfs",
    .readonly = false,
    .create = ram_create,
    .write = ram_write,
    .truncate = ram_truncate,
    .replace = ram_replace,
    .remove = ram_remove,
};

static int ro_create(struct vfs_node *p, const char *name, uint8_t type, struct vfs_node **out)
{
    UNUSED(p); UNUSED(name); UNUSED(type); UNUSED(out);
    return VFS_EROFS;
}
static int ro_write(struct vfs_node *n, uint32_t off, const void *b, uint32_t len)
{
    UNUSED(n); UNUSED(off); UNUSED(b); UNUSED(len);
    return VFS_EROFS;
}
static int ro_truncate(struct vfs_node *n, uint32_t s)
{
    UNUSED(n); UNUSED(s);
    return VFS_EROFS;
}
static int ro_replace(struct vfs_node *n, const void *b, uint32_t len)
{
    UNUSED(n); UNUSED(b); UNUSED(len);
    return VFS_EROFS;
}

/* Read-only backend for initramfs nodes and the synthetic root. */
const struct fs_backend initramfs_backend = {
    .name = "initramfs",
    .readonly = true,
    .create = ro_create,
    .write = ro_write,
    .truncate = ro_truncate,
    .replace = ro_replace,
};
