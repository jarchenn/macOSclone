/* VFS core: namespace, path lookup, open-file table and dispatch to backends. */
#include "fs/fs_internal.h"
#include "fs/initramfs.h"
#include "fs/pfs.h"
#include "kalloc.h"
#include "lib.h"

struct open_file {
    struct vfs_node *node;
    uint32_t offset;
    unsigned flags;
    bool used;
    bool wrote; /* commit to disk on close */
};

static struct vfs_node *root;
static struct vfs_node *home;
static struct open_file files[VFS_MAX_OPEN];
static uint32_t generation;
static struct pfs pfs;
static bool persistent;
static int last_commit_error;

uint32_t vfs_generation(void) { return generation; }

/* ---- persistent /home ----------------------------------------------------- */

static int commit_home(void);

static int p_create(struct vfs_node *parent, const char *name, uint8_t type, struct vfs_node **out);
static int p_write(struct vfs_node *n, uint32_t off, const void *buf, uint32_t len)
{
    int r = ramfs_backend.write(n, off, buf, len);
    if (!r)
        n->dirty = true;
    return r;
}
static int p_truncate(struct vfs_node *n, uint32_t size)
{
    int r = ramfs_backend.truncate(n, size);
    if (!r)
        n->dirty = true;
    return r;
}
static int p_replace(struct vfs_node *n, const void *buf, uint32_t len)
{
    int r = ramfs_backend.replace(n, buf, len);
    if (!r)
        n->dirty = true;
    return r;
}
static int p_remove(struct vfs_node *n) { return ramfs_backend.remove(n); }
static int p_sync(void) { return commit_home(); }

static const struct fs_backend pfs_backend = {
    .name = "pfs",
    .readonly = false,
    .create = p_create,
    .write = p_write,
    .truncate = p_truncate,
    .replace = p_replace,
    .remove = p_remove,
    .sync = p_sync,
};

static int p_create(struct vfs_node *parent, const char *name, uint8_t type, struct vfs_node **out)
{
    int r = ramfs_backend.create(parent, name, type, out);
    if (!r) {
        (*out)->fs = &pfs_backend;
        (*out)->dirty = true;
    }
    return r;
}

static int map_pfs_error(int r)
{
    switch (r) {
    case PFS_OK: return VFS_OK;
    case PFS_E_NOSPC:
    case PFS_E_TOOMANY: return VFS_ENOSPC;
    case PFS_E_NOMEM: return VFS_ENOMEM;
    default: return VFS_EIO;
    }
}

static int collect(struct vfs_node *dir, const char *prefix, struct pfs_entry *e, struct vfs_node **nodes, size_t *n)
{
    for (struct vfs_node *c = dir->children; c; c = c->next) {
        if (*n >= PFS_MAX_ENTRIES)
            return PFS_E_TOOMANY;
        struct pfs_entry *x = &e[*n];
        memset(x, 0, sizeof *x);
        if (prefix[0])
            k_snprintf(x->path, sizeof x->path, "%s/%s", prefix, c->name);
        else
            str_copy(x->path, c->name, sizeof x->path);
        x->type = c->type;
        if (c->type == VFS_FILE) {
            x->size = c->size;
            x->data = c->data;
            x->dirty = c->dirty;
            x->start = c->disk_start;
            x->nblocks = c->disk_blocks;
            x->crc = c->disk_crc;
        }
        nodes[(*n)++] = c;
        if (c->type == VFS_DIR) {
            int r = collect(c, x->path, e, nodes, n);
            if (r)
                return r;
        }
    }
    return PFS_OK;
}

static int commit_home(void)
{
    if (!persistent)
        return VFS_OK;
    struct pfs_entry *e = kcalloc(PFS_MAX_ENTRIES, sizeof *e);
    struct vfs_node **nodes = kcalloc(PFS_MAX_ENTRIES, sizeof *nodes);
    int r = PFS_E_NOMEM;
    size_t n = 0;
    if (e && nodes) {
        r = collect(home, "", e, nodes, &n);
        if (r == PFS_OK)
            r = pfs_commit(&pfs, e, n);
        if (r == PFS_OK) {
            for (size_t i = 0; i < n; i++) {
                if (nodes[i]->type == VFS_FILE) {
                    nodes[i]->disk_start = e[i].start;
                    nodes[i]->disk_blocks = e[i].nblocks;
                    nodes[i]->disk_crc = e[i].crc;
                    nodes[i]->dirty = false;
                }
            }
        }
    }
    if (e)
        kfree(e);
    if (nodes)
        kfree(nodes);
    last_commit_error = map_pfs_error(r);
    return last_commit_error;
}

static bool in_persistent_home(const struct vfs_node *n)
{
    if (!persistent)
        return false;
    for (; n; n = n->parent)
        if (n == home)
            return true;
    return false;
}

/* Called after a mutation completed in memory: makes it durable. */
static int persist(const struct vfs_node *n) { return in_persistent_home(n) ? commit_home() : VFS_OK; }

bool vfs_home_persistent(void) { return persistent; }

void vfs_storage(struct vfs_storage *out)
{
    memset(out, 0, sizeof *out);
    out->persistent = persistent;
    if (!persistent)
        return;
    out->generation = pfs.stats.generation;
    out->commits = pfs.stats.commits;
    out->blocks_written = pfs.stats.blocks_written;
    out->used_blocks = pfs.stats.used_blocks;
    out->total_blocks = pfs.stats.total_blocks;
    out->corrupt_files = (uint32_t)pfs.stats.corrupt_files;
    out->last_error = last_commit_error;
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
    default: return "unknown error";
    }
}

struct vfs_node *vfs_node_new(const char *name, uint8_t type, const struct fs_backend *fs)
{
    struct vfs_node *n = kzalloc(sizeof *n);
    if (!n)
        return NULL;
    str_copy(n->name, name, sizeof n->name);
    n->type = type;
    n->fs = fs;
    return n;
}

void vfs_node_attach(struct vfs_node *parent, struct vfs_node *child)
{
    child->parent = parent;
    struct vfs_node **pp = &parent->children;
    while (*pp && strcmp((*pp)->name, child->name) < 0)
        pp = &(*pp)->next;
    child->next = *pp;
    *pp = child;
}

static void free_tree(struct vfs_node *n)
{
    while (n) {
        struct vfs_node *next = n->next;
        free_tree(n->children);
        if (n->capacity && n->data)
            kfree(n->data);
        kfree(n);
        n = next;
    }
}

void vfs_init(void)
{
    if (persistent) {
        pfs_unmount(&pfs);
        persistent = false;
    }
    last_commit_error = 0;
    if (root)
        free_tree(root);
    memset(files, 0, sizeof files);
    ramfs_reset_usage();
    root = vfs_node_new("", VFS_DIR, &initramfs_backend);
    home = vfs_node_new("home", VFS_DIR, &ramfs_backend);
    if (root && home)
        vfs_node_attach(root, home);
    generation++;
}

static struct vfs_node *child_named(struct vfs_node *dir, const char *name, size_t n)
{
    for (struct vfs_node *c = dir->children; c; c = c->next)
        if (strlen(c->name) == n && memcmp(c->name, name, n) == 0)
            return c;
    return NULL;
}

/* Resolves a normalised absolute path. */
static int lookup(const char *norm, struct vfs_node **out)
{
    if (!root)
        return VFS_ENOENT;
    struct vfs_node *n = root;
    const char *p = norm;
    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        const char *s = p;
        while (*p && *p != '/')
            p++;
        if (n->type != VFS_DIR)
            return VFS_ENOTDIR;
        n = child_named(n, s, (size_t)(p - s));
        if (!n)
            return VFS_ENOENT;
    }
    *out = n;
    return VFS_OK;
}

static int resolve(const char *path, struct vfs_node **out, char *norm_out)
{
    char norm[VFS_PATH_MAX + 1];
    int r = vfs_normalize("/", path, norm, sizeof norm);
    if (r)
        return r;
    if (norm_out)
        str_copy(norm_out, norm, VFS_PATH_MAX + 1);
    return lookup(norm, out);
}

/* Creates the final component of `path` with the given type. */
static int create_node(const char *path, uint8_t type, struct vfs_node **out)
{
    char norm[VFS_PATH_MAX + 1], parent[VFS_PATH_MAX + 1], name[VFS_NAME_MAX + 1];
    int r = vfs_normalize("/", path, norm, sizeof norm);
    if (r)
        return r;
    struct vfs_node *existing;
    if (lookup(norm, &existing) == VFS_OK)
        return VFS_EEXIST;
    r = vfs_split(norm, parent, sizeof parent, name, sizeof name);
    if (r)
        return r;
    struct vfs_node *dir;
    r = lookup(parent, &dir);
    if (r)
        return r;
    if (dir->type != VFS_DIR)
        return VFS_ENOTDIR;
    r = dir->fs->create(dir, name, type, out);
    if (r == VFS_OK)
        generation++;
    return r;
}

int vfs_mkdir(const char *path)
{
    struct vfs_node *n;
    int r = create_node(path, VFS_DIR, &n);
    return r ? r : persist(n);
}

int vfs_create(const char *path)
{
    struct vfs_node *n;
    int r = create_node(path, VFS_FILE, &n);
    return r ? r : persist(n);
}

int vfs_stat(const char *path, struct vfs_stat *st)
{
    struct vfs_node *n;
    int r = resolve(path, &n, NULL);
    if (r)
        return r;
    st->type = n->type;
    st->readonly = n->fs->readonly;
    st->size = n->type == VFS_FILE ? n->size : 0;
    return VFS_OK;
}

int vfs_readdir(const char *path, uint32_t index, struct vfs_dirent *out)
{
    struct vfs_node *n;
    int r = resolve(path, &n, NULL);
    if (r)
        return r;
    if (n->type != VFS_DIR)
        return VFS_ENOTDIR;
    struct vfs_node *c = n->children;
    for (uint32_t i = 0; c && i < index; i++)
        c = c->next;
    if (!c)
        return VFS_ENOENT;
    str_copy(out->name, c->name, sizeof out->name);
    out->type = c->type;
    out->readonly = c->fs->readonly;
    out->size = c->type == VFS_FILE ? c->size : 0;
    return VFS_OK;
}

int vfs_open(const char *path, unsigned flags)
{
    if (!(flags & (VFS_O_READ | VFS_O_WRITE)))
        return VFS_EINVAL;
    int slot = -1;
    for (int i = 0; i < VFS_MAX_OPEN; i++)
        if (!files[i].used) {
            slot = i;
            break;
        }
    if (slot < 0)
        return VFS_EMFILE;
    struct vfs_node *n;
    bool created = false;
    int r = resolve(path, &n, NULL);
    if (r == VFS_ENOENT && (flags & VFS_O_CREATE)) {
        r = create_node(path, VFS_FILE, &n);
        created = r == VFS_OK;
    }
    if (r)
        return r;
    if (n->type == VFS_DIR)
        return VFS_EISDIR;
    if ((flags & (VFS_O_WRITE | VFS_O_TRUNC)) && n->fs->readonly)
        return VFS_EROFS;
    if (flags & VFS_O_TRUNC) {
        r = n->fs->truncate(n, 0);
        if (r)
            return r;
        generation++;
    }
    files[slot] = (struct open_file){ .node = n, .offset = 0, .flags = flags, .used = true,
                                      .wrote = created || (flags & VFS_O_TRUNC) };
    return slot;
}

static struct open_file *get_fd(int fd)
{
    if (fd < 0 || fd >= VFS_MAX_OPEN || !files[fd].used)
        return NULL;
    return &files[fd];
}

int vfs_close(int fd)
{
    struct open_file *f = get_fd(fd);
    if (!f)
        return VFS_EBADF;
    struct vfs_node *n = f->node;
    bool wrote = f->wrote;
    f->used = false;
    f->node = NULL;
    return wrote ? persist(n) : VFS_OK;
}

int vfs_read(int fd, void *buf, size_t n)
{
    struct open_file *f = get_fd(fd);
    if (!f || !(f->flags & VFS_O_READ))
        return VFS_EBADF;
    struct vfs_node *node = f->node;
    if (f->offset >= node->size)
        return 0;
    size_t avail = node->size - f->offset;
    if (n > avail)
        n = avail;
    if (n > INT32_MAX)
        n = INT32_MAX;
    memcpy(buf, node->data + f->offset, n);
    f->offset += (uint32_t)n;
    return (int)n;
}

int vfs_write(int fd, const void *buf, size_t n)
{
    struct open_file *f = get_fd(fd);
    if (!f || !(f->flags & VFS_O_WRITE))
        return VFS_EBADF;
    if (n > RAMFS_MAX_FILE)
        return VFS_EFBIG;
    int r = f->node->fs->write(f->node, f->offset, buf, (uint32_t)n);
    if (r)
        return r;
    f->wrote = true;
    f->offset += (uint32_t)n;
    generation++;
    return (int)n;
}

int vfs_seek(int fd, uint32_t offset)
{
    struct open_file *f = get_fd(fd);
    if (!f)
        return VFS_EBADF;
    f->offset = offset;
    return VFS_OK;
}

int vfs_truncate(const char *path, uint32_t size)
{
    struct vfs_node *n;
    int r = resolve(path, &n, NULL);
    if (r)
        return r;
    if (n->type == VFS_DIR)
        return VFS_EISDIR;
    r = n->fs->truncate(n, size);
    if (r == VFS_OK) {
        generation++;
        r = persist(n);
    }
    return r;
}

static void detach(struct vfs_node *n)
{
    struct vfs_node **pp = &n->parent->children;
    while (*pp && *pp != n)
        pp = &(*pp)->next;
    if (*pp)
        *pp = n->next;
    n->next = NULL;
    n->parent = NULL;
}

static bool node_open(const struct vfs_node *n)
{
    for (int i = 0; i < VFS_MAX_OPEN; i++)
        if (files[i].used && files[i].node == n)
            return true;
    return false;
}

int vfs_remove(const char *path)
{
    struct vfs_node *n;
    int r = resolve(path, &n, NULL);
    if (r)
        return r;
    if (n == root || n == home || !n->parent)
        return VFS_EINVAL; /* mount points cannot be removed */
    if (n->fs->readonly)
        return VFS_EROFS;
    if (n->type == VFS_DIR && n->children)
        return VFS_ENOTEMPTY;
    if (node_open(n))
        return VFS_EBUSY;
    r = n->fs->remove ? n->fs->remove(n) : VFS_EROFS;
    if (r)
        return r;
    struct vfs_node *parent = n->parent;
    detach(n);
    if (n->capacity && n->data)
        kfree(n->data);
    kfree(n);
    generation++;
    return persist(parent);
}

int vfs_rename(const char *from, const char *to)
{
    char nto[VFS_PATH_MAX + 1], parent[VFS_PATH_MAX + 1], name[VFS_NAME_MAX + 1];
    struct vfs_node *n, *dir, *existing;
    int r = resolve(from, &n, NULL);
    if (r)
        return r;
    if (n == root || n == home || !n->parent)
        return VFS_EINVAL;
    if (n->fs->readonly)
        return VFS_EROFS;
    r = vfs_normalize("/", to, nto, sizeof nto);
    if (r)
        return r;
    if (lookup(nto, &existing) == VFS_OK)
        return existing == n ? VFS_OK : VFS_EEXIST;
    r = vfs_split(nto, parent, sizeof parent, name, sizeof name);
    if (r)
        return r;
    r = lookup(parent, &dir);
    if (r)
        return r;
    if (dir->type != VFS_DIR)
        return VFS_ENOTDIR;
    if (dir->fs != n->fs)
        return VFS_EROFS; /* cross-filesystem moves are not supported */
    /* A directory cannot be moved inside itself. */
    for (struct vfs_node *a = dir; a; a = a->parent)
        if (a == n)
            return VFS_EINVAL;
    detach(n);
    str_copy(n->name, name, sizeof n->name);
    vfs_node_attach(dir, n);
    if (n->fs->changed)
        n->fs->changed(n);
    generation++;
    return persist(n);
}

int vfs_sync(void)
{
    return home && home->fs->sync ? home->fs->sync() : VFS_OK;
}

int vfs_read_file(const char *path, void *buf, size_t cap, size_t *len)
{
    struct vfs_node *n;
    int r = resolve(path, &n, NULL);
    if (r)
        return r;
    if (n->type == VFS_DIR)
        return VFS_EISDIR;
    if (n->size > cap)
        return VFS_EFBIG;
    memcpy(buf, n->data, n->size);
    *len = n->size;
    return VFS_OK;
}

int vfs_write_file(const char *path, const void *data, size_t len)
{
    if (len > RAMFS_MAX_FILE)
        return VFS_EFBIG;
    struct vfs_node *n;
    int r = resolve(path, &n, NULL);
    if (r == VFS_ENOENT) {
        r = create_node(path, VFS_FILE, &n);
        if (r)
            return r;
        r = n->fs->replace(n, data, (uint32_t)len);
        generation++;
        return r ? r : persist(n);
    }
    if (r)
        return r;
    if (n->type == VFS_DIR)
        return VFS_EISDIR;
    r = n->fs->replace(n, data, (uint32_t)len);
    if (r == VFS_OK) {
        generation++;
        r = persist(n);
    }
    return r;
}

void vfs_usage(struct vfs_usage *u)
{
    u->ram_bytes = ramfs_bytes_used();
    u->ram_nodes = ramfs_nodes_used();
    u->open_files = 0;
    for (int i = 0; i < VFS_MAX_OPEN; i++)
        if (files[i].used)
            u->open_files++;
}

/* ---- initramfs loading --------------------------------------------------- */

static int load_entry(void *ctx, uint32_t type, const char *path, const uint8_t *data, uint64_t len)
{
    UNUSED(ctx);
    char abs[VFS_PATH_MAX + 2];
    abs[0] = '/';
    str_copy(abs + 1, path, sizeof abs - 1);
    bool to_home = str_starts_with(path, "home/") || strcmp(path, "home") == 0;
    if (strcmp(path, "home") == 0)
        return VFS_OK; /* /home already exists (RAM fs) */
    if (to_home) {
        /* Seed the writable RAM filesystem with a copy. */
        if (type == VFS_DIR) {
            int r = vfs_mkdir(abs);
            return r == VFS_EEXIST ? VFS_OK : r;
        }
        return vfs_write_file(abs, data, (size_t)len);
    }
    /* Read-only node pointing into the archive. */
    char parent[VFS_PATH_MAX + 1], name[VFS_NAME_MAX + 1];
    int r = vfs_split(abs, parent, sizeof parent, name, sizeof name);
    if (r)
        return r;
    struct vfs_node *dir, *existing;
    r = lookup(parent, &dir);
    if (r)
        return VFS_EBADARCHIVE; /* parents must precede children */
    if (dir->type != VFS_DIR || dir->fs != &initramfs_backend)
        return VFS_EBADARCHIVE;
    if (lookup(abs, &existing) == VFS_OK)
        return VFS_EBADARCHIVE; /* duplicate entry */
    struct vfs_node *n = vfs_node_new(name, (uint8_t)type, &initramfs_backend);
    if (!n)
        return VFS_ENOMEM;
    if (type == VFS_FILE) {
        if (len > UINT32_MAX)
            return VFS_EBADARCHIVE;
        n->data = (uint8_t *)data;
        n->size = (uint32_t)len;
    }
    vfs_node_attach(dir, n);
    return VFS_OK;
}

/* ---- attaching the data disk ------------------------------------------- */

static int emit_entry(void *ctx, const struct pfs_entry *e, const uint8_t *data)
{
    UNUSED(ctx);
    char abs[VFS_PATH_MAX + 2], parent[VFS_PATH_MAX + 1], name[VFS_NAME_MAX + 1];
    k_snprintf(abs, sizeof abs, "/home/%s", e->path);
    char norm[VFS_PATH_MAX + 1];
    if (vfs_normalize("/", abs, norm, sizeof norm) || vfs_split(norm, parent, sizeof parent, name, sizeof name))
        return -1;
    struct vfs_node *dir, *existing;
    if (lookup(parent, &dir) || dir->type != VFS_DIR || lookup(norm, &existing) == VFS_OK)
        return -1;
    struct vfs_node *n;
    if (ramfs_backend.create(dir, name, e->type, &n))
        return -1;
    n->fs = &pfs_backend;
    if (e->type == VFS_FILE) {
        if (ramfs_backend.replace(n, data, e->size)) {
            /* Out of RAM quota: drop the node rather than ever committing an
             * empty copy over the data on disk. */
            ramfs_backend.remove(n);
            detach(n);
            kfree(n);
            return -1;
        }
        n->disk_start = e->start;
        n->disk_blocks = e->nblocks;
        n->disk_crc = e->crc;
    }
    n->dirty = false;
    return 0;
}

static void convert_tree(struct vfs_node *n)
{
    for (; n; n = n->next) {
        n->fs = &pfs_backend;
        n->dirty = true;
        convert_tree(n->children);
    }
}

static void drop_tree(struct vfs_node *n)
{
    while (n) {
        struct vfs_node *next = n->next;
        drop_tree(n->children);
        ramfs_backend.remove(n); /* release RAM quota */
        if (n->capacity && n->data)
            kfree(n->data);
        kfree(n);
        n = next;
    }
}

int vfs_attach_persistent(struct pfs_blockdev *dev, bool *formatted_out)
{
    if (!home || !dev)
        return VFS_EINVAL;
    /* Contents seeded from the initramfs stay aside until we know whether
     * the disk is new (keep them) or already holds the user's files. */
    struct vfs_node *seed = home->children;
    home->children = NULL;
    bool formatted = false;
    int r = pfs_mount(&pfs, dev, emit_entry, NULL, &formatted);
    if (r != PFS_OK) {
        drop_tree(home->children);
        home->children = seed;
        return map_pfs_error(r);
    }
    persistent = true;
    home->fs = &pfs_backend;
    if (formatted) {
        home->children = seed;
        convert_tree(seed);
        r = commit_home();
    } else {
        drop_tree(seed);
        r = VFS_OK;
    }
    if (formatted_out)
        *formatted_out = formatted;
    generation++;
    return r;
}

int vfs_load_initramfs(const uint8_t *data, size_t size)
{
    int r = initramfs_parse(data, size, load_entry, NULL);
    generation++;
    return r;
}
