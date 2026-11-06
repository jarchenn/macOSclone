/* Aurora persistent filesystem core (see fs/pfs.h for the design). */
#include "fs/pfs.h"
#include "kalloc.h"
#include "lib.h"

#define SB_MAGIC "AURPFS01"
#define CAT_MAGIC 0x4C544143u /* "CATL" */
#define ENTRY_HDR 16u

/* Superblock (first 64 bytes of a superblock block, rest zero):
 *   0  char[8] magic   8 u32 version   12 u32 block_size
 *  16 u64 generation  24 u64 total_blocks
 *  32 u32 cat_start   36 u32 cat_blocks  40 u32 cat_bytes  44 u32 cat_crc
 *  48 u32 entry_count 52 u32 reserved    56 u32 reserved   60 u32 sb_crc (bytes 0..59)
 * Catalog: u32 magic, u32 count, then entries:
 *   u8 type, u8 0, u16 path_len, u32 size, u32 start, u32 crc, path bytes. */

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t get64(const uint8_t *p) { return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32; }

static bool bit(const uint8_t *m, uint64_t b) { return (m[b / 8] >> (b % 8)) & 1; }
static void setbit(uint8_t *m, uint64_t b) { m[b / 8] |= (uint8_t)(1u << (b % 8)); }

const char *pfs_strerror(int err)
{
    switch (err) {
    case PFS_OK: return "ok";
    case PFS_E_IO: return "disk I/O error";
    case PFS_E_NOSPC: return "disk full";
    case PFS_E_CORRUPT: return "filesystem corrupt";
    case PFS_E_TOOMANY: return "too many files";
    case PFS_E_SMALL: return "disk too small";
    case PFS_E_NOMEM: return "out of memory";
    default: return "unknown error";
    }
}

static uint32_t blocks_for(uint64_t bytes) { return (uint32_t)((bytes + PFS_BLOCK - 1) / PFS_BLOCK); }

/* Finds `count` contiguous blocks free in both bitmaps and marks them pending. */
static int alloc_extent(struct pfs *fs, uint32_t count, uint32_t *start)
{
    if (count == 0) {
        *start = 0;
        return PFS_OK;
    }
    uint64_t run = 0;
    for (uint64_t b = 2; b < fs->nblocks; b++) {
        run = (bit(fs->committed, b) || bit(fs->pending, b)) ? 0 : run + 1;
        if (run == count) {
            uint64_t first = b + 1 - count;
            for (uint64_t k = first; k <= b; k++)
                setbit(fs->pending, k);
            *start = (uint32_t)first;
            return PFS_OK;
        }
    }
    return PFS_E_NOSPC;
}

static int write_bytes(struct pfs *fs, uint32_t start, const uint8_t *data, uint64_t len)
{
    for (uint32_t i = 0; i < blocks_for(len); i++) {
        uint64_t off = (uint64_t)i * PFS_BLOCK;
        uint64_t n = MIN((uint64_t)PFS_BLOCK, len - off);
        memset(fs->buf, 0, PFS_BLOCK);
        memcpy(fs->buf, data + off, n);
        if (fs->dev->write(fs->dev->ctx, start + i, fs->buf))
            return PFS_E_IO;
        fs->stats.blocks_written++;
    }
    return PFS_OK;
}

static int read_bytes(struct pfs *fs, uint32_t start, uint8_t *data, uint64_t len)
{
    for (uint32_t i = 0; i < blocks_for(len); i++) {
        uint64_t off = (uint64_t)i * PFS_BLOCK;
        if ((uint64_t)start + i >= fs->nblocks)
            return PFS_E_CORRUPT;
        if (fs->dev->read(fs->dev->ctx, start + i, fs->buf))
            return PFS_E_IO;
        memcpy(data + off, fs->buf, MIN((uint64_t)PFS_BLOCK, len - off));
    }
    return PFS_OK;
}

static void encode_sb(struct pfs *fs, uint8_t *b, uint64_t gen, uint32_t cs, uint32_t cb, uint32_t cbytes, uint32_t ccrc, uint32_t count)
{
    memset(b, 0, PFS_BLOCK);
    memcpy(b, SB_MAGIC, 8);
    put32(b + 8, 1);
    put32(b + 12, PFS_BLOCK);
    put64(b + 16, gen);
    put64(b + 24, fs->nblocks);
    put32(b + 32, cs);
    put32(b + 36, cb);
    put32(b + 40, cbytes);
    put32(b + 44, ccrc);
    put32(b + 48, count);
    put32(b + 60, crc32(b, 60));
}

struct sb {
    bool valid;
    uint64_t gen;
    uint32_t cat_start, cat_blocks, cat_bytes, cat_crc, count;
};

static struct sb decode_sb(struct pfs *fs, const uint8_t *b)
{
    struct sb s;
    memset(&s, 0, sizeof s);
    if (memcmp(b, SB_MAGIC, 8) || get32(b + 8) != 1 || get32(b + 12) != PFS_BLOCK)
        return s;
    if (crc32(b, 60) != get32(b + 60))
        return s;
    s.gen = get64(b + 16);
    s.cat_start = get32(b + 32);
    s.cat_blocks = get32(b + 36);
    s.cat_bytes = get32(b + 40);
    s.cat_crc = get32(b + 44);
    s.count = get32(b + 48);
    uint64_t total = get64(b + 24);
    if (total != fs->nblocks || s.count > PFS_MAX_ENTRIES || s.cat_blocks != blocks_for(s.cat_bytes) ||
        s.cat_start < 2 || (uint64_t)s.cat_start + s.cat_blocks > fs->nblocks || s.cat_bytes < 8)
        return s;
    s.valid = true;
    return s;
}

static size_t catalog_size(const struct pfs_entry *e, size_t n)
{
    size_t sz = 8;
    for (size_t i = 0; i < n; i++)
        sz += ENTRY_HDR + strlen(e[i].path);
    return sz;
}

int pfs_commit(struct pfs *fs, struct pfs_entry *e, size_t n)
{
    if (n > PFS_MAX_ENTRIES)
        return PFS_E_TOOMANY;
    size_t mapbytes = (size_t)((fs->nblocks + 7) / 8);
    memset(fs->pending, 0, mapbytes);
    setbit(fs->pending, 0);
    setbit(fs->pending, 1);
    /* Keep the extents of clean files; remember where dirty files will go. */
    uint32_t *new_start = kcalloc(n ? n : 1, sizeof(uint32_t));
    if (!new_start)
        return PFS_E_NOMEM;
    int r = PFS_OK;
    for (size_t i = 0; i < n; i++) {
        if (e[i].type == 1 && !e[i].dirty && e[i].size)
            for (uint32_t k = 0; k < e[i].nblocks; k++)
                setbit(fs->pending, (uint64_t)e[i].start + k);
    }
    for (size_t i = 0; i < n && r == PFS_OK; i++) {
        new_start[i] = e[i].start;
        if (e[i].type != 1 || !e[i].dirty)
            continue;
        if (e[i].size > PFS_MAX_FILE) {
            r = PFS_E_NOSPC;
            break;
        }
        r = alloc_extent(fs, blocks_for(e[i].size), &new_start[i]);
        if (r == PFS_OK)
            r = write_bytes(fs, new_start[i], e[i].data, e[i].size);
    }
    /* Catalog */
    size_t cbytes = catalog_size(e, n);
    uint8_t *cat = r == PFS_OK ? kmalloc(cbytes) : NULL;
    if (r == PFS_OK && !cat)
        r = PFS_E_NOMEM;
    uint32_t cstart = 0;
    if (r == PFS_OK) {
        put32(cat, CAT_MAGIC);
        put32(cat + 4, (uint32_t)n);
        size_t off = 8;
        for (size_t i = 0; i < n; i++) {
            size_t pl = strlen(e[i].path);
            uint32_t crc = e[i].dirty ? crc32(e[i].data, e[i].size) : e[i].crc;
            cat[off] = e[i].type;
            cat[off + 1] = 0;
            cat[off + 2] = (uint8_t)pl;
            cat[off + 3] = (uint8_t)(pl >> 8);
            put32(cat + off + 4, e[i].type == 1 ? e[i].size : 0);
            put32(cat + off + 8, e[i].type == 1 ? new_start[i] : 0);
            put32(cat + off + 12, e[i].type == 1 ? crc : 0);
            memcpy(cat + off + ENTRY_HDR, e[i].path, pl);
            off += ENTRY_HDR + pl;
        }
        r = alloc_extent(fs, blocks_for(cbytes), &cstart);
        if (r == PFS_OK)
            r = write_bytes(fs, cstart, cat, cbytes);
    }
    /* Barrier, then the superblock in the other slot, then another barrier. */
    if (r == PFS_OK && fs->dev->flush(fs->dev->ctx))
        r = PFS_E_IO;
    int slot = fs->slot ^ 1;
    if (r == PFS_OK) {
        encode_sb(fs, fs->buf, fs->generation + 1, cstart, blocks_for(cbytes), (uint32_t)cbytes,
                  crc32(cat, cbytes), (uint32_t)n);
        if (fs->dev->write(fs->dev->ctx, (uint64_t)slot, fs->buf) || fs->dev->flush(fs->dev->ctx))
            r = PFS_E_IO;
        else
            fs->stats.blocks_written++;
    }
    if (r == PFS_OK) {
        fs->generation++;
        fs->slot = slot;
        memcpy(fs->committed, fs->pending, mapbytes);
        for (size_t i = 0; i < n; i++) {
            if (e[i].type == 1 && e[i].dirty) {
                e[i].crc = crc32(e[i].data, e[i].size);
                e[i].start = new_start[i];
                e[i].nblocks = blocks_for(e[i].size);
                e[i].dirty = false;
            }
        }
        fs->stats.commits++;
        fs->stats.generation = fs->generation;
        uint32_t used = 0;
        for (uint64_t b = 0; b < fs->nblocks; b++)
            used += bit(fs->committed, b);
        fs->stats.used_blocks = used;
    }
    if (cat)
        kfree(cat);
    kfree(new_start);
    return r;
}

static int format(struct pfs *fs)
{
    /* Generation 0 lives in "slot 1" so the first commit lands in slot 0. */
    fs->generation = 0;
    fs->slot = 1;
    memset(fs->committed, 0, (size_t)((fs->nblocks + 7) / 8));
    setbit(fs->committed, 0);
    setbit(fs->committed, 1);
    /* Invalidate both slots, then commit an empty catalog. */
    memset(fs->buf, 0, PFS_BLOCK);
    if (fs->dev->write(fs->dev->ctx, 0, fs->buf) || fs->dev->write(fs->dev->ctx, 1, fs->buf))
        return PFS_E_IO;
    return pfs_commit(fs, NULL, 0);
}

int pfs_mount(struct pfs *fs, struct pfs_blockdev *dev, pfs_emit_fn emit, void *ctx, bool *formatted)
{
    memset(fs, 0, sizeof *fs);
    fs->dev = dev;
    fs->nblocks = dev->nblocks;
    if (formatted)
        *formatted = false;
    if (fs->nblocks < PFS_MIN_BLOCKS)
        return PFS_E_SMALL;
    size_t mapbytes = (size_t)((fs->nblocks + 7) / 8);
    fs->committed = kzalloc(mapbytes);
    fs->pending = kzalloc(mapbytes);
    fs->buf = kmalloc(PFS_BLOCK);
    if (!fs->committed || !fs->pending || !fs->buf) {
        pfs_unmount(fs);
        return PFS_E_NOMEM;
    }
    struct sb s[2];
    bool any_magic = false;
    for (int i = 0; i < 2; i++) {
        if (dev->read(dev->ctx, (uint64_t)i, fs->buf)) {
            pfs_unmount(fs);
            return PFS_E_IO;
        }
        any_magic |= memcmp(fs->buf, SB_MAGIC, 8) == 0;
        s[i] = decode_sb(fs, fs->buf);
    }
    /* Try the newest valid superblock first, then the other one. */
    int order[2] = { 0, 1 };
    if (s[1].valid && (!s[0].valid || s[1].gen > s[0].gen)) {
        order[0] = 1;
        order[1] = 0;
    }
    for (int k = 0; k < 2; k++) {
        struct sb *b = &s[order[k]];
        if (!b->valid)
            continue;
        uint8_t *cat = kmalloc(b->cat_bytes);
        if (!cat) {
            pfs_unmount(fs);
            return PFS_E_NOMEM;
        }
        int r = read_bytes(fs, b->cat_start, cat, b->cat_bytes);
        if (r == PFS_OK && (crc32(cat, b->cat_bytes) != b->cat_crc || get32(cat) != CAT_MAGIC || get32(cat + 4) != b->count))
            r = PFS_E_CORRUPT;
        if (r != PFS_OK) {
            kfree(cat);
            if (r == PFS_E_IO) {
                pfs_unmount(fs);
                return r;
            }
            continue; /* this superblock's catalog is bad: fall back */
        }
        memset(fs->committed, 0, mapbytes);
        setbit(fs->committed, 0);
        setbit(fs->committed, 1);
        for (uint32_t i = 0; i < b->cat_blocks; i++)
            setbit(fs->committed, (uint64_t)b->cat_start + i);
        /* Walk entries: validate, mark extents, load and emit. */
        size_t off = 8;
        uint8_t *data = kmalloc(PFS_MAX_FILE);
        struct pfs_entry *ent = kmalloc(sizeof *ent);
        if (!data || !ent) {
            kfree(cat);
            if (data) kfree(data);
            if (ent) kfree(ent);
            pfs_unmount(fs);
            return PFS_E_NOMEM;
        }
        r = PFS_OK;
        for (uint32_t i = 0; i < b->count && r == PFS_OK; i++) {
            if (off + ENTRY_HDR > b->cat_bytes) {
                r = PFS_E_CORRUPT;
                break;
            }
            memset(ent, 0, sizeof *ent);
            ent->type = cat[off];
            uint32_t pl = (uint32_t)cat[off + 2] | (uint32_t)cat[off + 3] << 8;
            ent->size = get32(cat + off + 4);
            ent->start = get32(cat + off + 8);
            ent->crc = get32(cat + off + 12);
            if ((ent->type != 1 && ent->type != 2) || pl == 0 || pl > PFS_MAX_PATH ||
                off + ENTRY_HDR + pl > b->cat_bytes || ent->size > PFS_MAX_FILE) {
                r = PFS_E_CORRUPT;
                break;
            }
            memcpy(ent->path, cat + off + ENTRY_HDR, pl);
            ent->path[pl] = '\0';
            off += ENTRY_HDR + pl;
            if (ent->type == 1) {
                ent->nblocks = blocks_for(ent->size);
                if (ent->size && (ent->start < 2 || (uint64_t)ent->start + ent->nblocks > fs->nblocks)) {
                    r = PFS_E_CORRUPT;
                    break;
                }
                for (uint32_t q = 0; q < ent->nblocks; q++)
                    setbit(fs->committed, (uint64_t)ent->start + q);
                int rr = read_bytes(fs, ent->start, data, ent->size);
                if (rr == PFS_E_IO) {
                    r = rr;
                    break;
                }
                if (rr != PFS_OK || crc32(data, ent->size) != ent->crc) {
                    fs->stats.corrupt_files++;
                    continue; /* skip a damaged file, keep the rest */
                }
            }
            if (emit && emit(ctx, ent, ent->type == 1 ? data : NULL) != 0) {
                /* the consumer rejected the entry (e.g. limits); keep going */
            }
        }
        kfree(data);
        kfree(ent);
        kfree(cat);
        if (r == PFS_E_IO) {
            pfs_unmount(fs);
            return r;
        }
        if (r != PFS_OK)
            continue;
        fs->generation = b->gen;
        fs->slot = order[k];
        fs->stats.mounts++;
        fs->stats.generation = fs->generation;
        fs->stats.total_blocks = (uint32_t)fs->nblocks;
        uint32_t used = 0;
        for (uint64_t q = 0; q < fs->nblocks; q++)
            used += bit(fs->committed, q);
        fs->stats.used_blocks = used;
        return PFS_OK;
    }
    /* No usable superblock. A blank disk is formatted; a disk that carries
     * Aurora superblocks but no readable state is left alone. */
    if (any_magic) {
        pfs_unmount(fs);
        return PFS_E_CORRUPT;
    }
    int r = format(fs);
    if (r == PFS_OK && formatted)
        *formatted = true;
    fs->stats.total_blocks = (uint32_t)fs->nblocks;
    return r;
}

void pfs_unmount(struct pfs *fs)
{
    if (fs->committed)
        kfree(fs->committed);
    if (fs->pending)
        kfree(fs->pending);
    if (fs->buf)
        kfree(fs->buf);
    fs->committed = fs->pending = fs->buf = NULL;
}
