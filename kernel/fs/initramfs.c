#include "fs/initramfs.h"
#include "fs/vfs.h"
#include "lib.h"

#define HEADER_SIZE 32u
#define ENTRY_HEADER 16u
#define MAX_ENTRIES 4096u

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }

/* Archive paths are relative ("system/about.txt"), no leading slash. */
static bool valid_archive_path(const char *p, size_t n)
{
    if (n == 0 || n > VFS_PATH_MAX - 1 || p[0] == '/')
        return false;
    size_t start = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i == n || p[i] == '/') {
            if (!vfs_valid_name(p + start, i - start))
                return false;
            start = i + 1;
        } else if (p[i] == '\0') {
            return false;
        }
    }
    return true;
}

int initramfs_parse(const uint8_t *a, size_t size, initramfs_cb cb, void *ctx)
{
    if (!a || size < HEADER_SIZE)
        return VFS_EBADARCHIVE;
    if (memcmp(a, "AURFS01\0", 8) != 0)
        return VFS_EBADARCHIVE;
    if (rd32(a + 8) != 1)
        return VFS_EBADARCHIVE;
    uint32_t count = rd32(a + 12);
    uint64_t total = rd64(a + 16);
    uint32_t crc = rd32(a + 24);
    if (count > MAX_ENTRIES || total < HEADER_SIZE || total > size)
        return VFS_EBADARCHIVE;
    if (crc32(a + HEADER_SIZE, (size_t)(total - HEADER_SIZE)) != crc)
        return VFS_EBADARCHIVE;

    uint64_t off = HEADER_SIZE;
    for (uint32_t i = 0; i < count; i++) {
        if (total - off < ENTRY_HEADER)
            return VFS_EBADARCHIVE;
        uint32_t type = rd32(a + off);
        uint32_t plen = rd32(a + off + 4);
        uint64_t dlen = rd64(a + off + 8);
        off += ENTRY_HEADER;
        if (type != 1 && type != 2) /* archive: 1 = directory, 2 = file */
            return VFS_EBADARCHIVE;
        if (plen == 0 || plen > VFS_PATH_MAX - 1)
            return VFS_EBADARCHIVE;
        uint64_t ppad = ALIGN_UP((uint64_t)plen, 8);
        if (total - off < ppad)
            return VFS_EBADARCHIVE;
        char path[VFS_PATH_MAX + 1];
        memcpy(path, a + off, plen);
        path[plen] = '\0';
        if (!valid_archive_path(path, plen))
            return VFS_EBADARCHIVE;
        off += ppad;
        if (type == 1 && dlen != 0)
            return VFS_EBADARCHIVE;
        if (dlen > total - off)
            return VFS_EBADARCHIVE;
        uint64_t dpad = ALIGN_UP(dlen, 8);
        if (dpad > total - off)
            return VFS_EBADARCHIVE;
        int r = cb(ctx, type == 1 ? VFS_DIR : VFS_FILE, path, a + off, dlen);
        if (r != VFS_OK)
            return r;
        off += dpad;
    }
    if (off != total)
        return VFS_EBADARCHIVE; /* trailing garbage inside the declared size */
    return VFS_OK;
}
