#ifndef AURORA_FS_INTERNAL_H
#define AURORA_FS_INTERNAL_H
#include "fs/vfs.h"

struct vfs_node;

/* Storage backend operations. Reads are served directly from node->data by
 * the VFS; backends implement mutation and enforce their own limits. */
struct fs_backend {
    const char *name;
    bool readonly;
    /* Create a child node (already validated name, parent is a dir). */
    int (*create)(struct vfs_node *parent, const char *name, uint8_t type, struct vfs_node **out);
    /* Write n bytes at off, growing the file as needed. */
    int (*write)(struct vfs_node *node, uint32_t off, const void *buf, uint32_t n);
    int (*truncate)(struct vfs_node *node, uint32_t size);
    /* Replace the whole content atomically. */
    int (*replace)(struct vfs_node *node, const void *buf, uint32_t n);
    /* Optional: called before a node is unlinked (frees backend storage). */
    int (*remove)(struct vfs_node *node);
    /* Optional: metadata changed (rename) - persistent backends record it. */
    void (*changed)(struct vfs_node *node);
    /* Optional: make all changes durable. */
    int (*sync)(void);
};

struct vfs_node {
    char name[VFS_NAME_MAX + 1];
    uint8_t type;
    const struct fs_backend *fs;
    struct vfs_node *parent;
    struct vfs_node *children; /* sorted by name */
    struct vfs_node *next;
    uint8_t *data;
    uint32_t size;
    uint32_t capacity; /* bytes owned by the node (0 for initramfs data) */
};

extern const struct fs_backend ramfs_backend;
extern const struct fs_backend initramfs_backend;

struct vfs_node *vfs_node_new(const char *name, uint8_t type, const struct fs_backend *fs);
void vfs_node_attach(struct vfs_node *parent, struct vfs_node *child);
void ramfs_reset_usage(void);
uint32_t ramfs_bytes_used(void);
uint32_t ramfs_nodes_used(void);

#endif
