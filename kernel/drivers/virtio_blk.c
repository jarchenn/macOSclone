/* virtio-blk over legacy PCI. One request in flight at a time; data goes
 * through a physically contiguous bounce buffer. */
#include "drivers/blk.h"
#include "drivers/virtio.h"
#include "arch/x86_64/io.h"
#include "core/time.h"
#include "core/log.h"
#include "fs/pfs.h"
#include "lib.h"

#define VIRTIO_BLK_T_IN 0
#define VIRTIO_BLK_T_OUT 1
#define VIRTIO_BLK_T_FLUSH 4
#define VIRTIO_BLK_F_RO (1u << 5)
#define VIRTIO_BLK_F_FLUSH (1u << 9)
#define MAX_SECTORS 128 /* 64 KiB per request */
#define TIMEOUT_MS 5000

struct PACKED req_hdr {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

static struct virtio_dev dev;
static struct virtq vq;
static bool present;
static uint64_t sectors;
static uint8_t *dma;           /* [0..15] header, [16] status, [4096..] data */
static phys_addr_t dma_phys;
static uint64_t nreq, nerr;

bool vblk_present(void) { return present; }
uint64_t vblk_sectors(void) { return sectors; }
bool vblk_readonly(void) { return (dev.features & VIRTIO_BLK_F_RO) != 0; }
uint64_t vblk_requests(void) { return nreq; }
uint64_t vblk_errors(void) { return nerr; }

bool vblk_init(void)
{
    const struct pci_dev *p = pci_find(0x1AF4, 0x1001);
    if (!p)
        return false;
    if (!virtio_begin(&dev, p, VIRTIO_BLK_F_FLUSH | VIRTIO_BLK_F_RO)) {
        klog("vblk: device has no legacy I/O BAR");
        return false;
    }
    if (!virtq_setup(&dev, &vq, 0, 256)) {
        klog("vblk: virtqueue setup failed");
        outb(dev.io + VIRTIO_REG_STATUS, VIRTIO_STATUS_FAILED);
        return false;
    }
    dma = dma_alloc(1 + MAX_SECTORS * 512 / 4096, &dma_phys);
    if (!dma) {
        outb(dev.io + VIRTIO_REG_STATUS, VIRTIO_STATUS_FAILED);
        return false;
    }
    sectors = (uint64_t)inl(dev.io + VIRTIO_REG_CONFIG) | (uint64_t)inl(dev.io + VIRTIO_REG_CONFIG + 4) << 32;
    virtio_finish(&dev);
    present = true;
    klog("vblk: %llu MiB disk, queue %u, flush %s%s", (unsigned long long)(sectors / 2048), vq.num,
         (dev.features & VIRTIO_BLK_F_FLUSH) ? "yes" : "no", vblk_readonly() ? ", read-only" : "");
    return true;
}

static int do_request(uint32_t type, uint64_t sector, uint32_t count, bool write_data)
{
    struct req_hdr *h = (struct req_hdr *)dma;
    h->type = type;
    h->reserved = 0;
    h->sector = sector;
    dma[16] = 0xFF; /* status: device overwrites */
    struct virtio_buf bufs[3];
    int n = 0;
    bufs[n++] = (struct virtio_buf){ dma_phys, sizeof *h, false };
    if (count)
        bufs[n++] = (struct virtio_buf){ dma_phys + 4096, count * 512u, !write_data };
    bufs[n++] = (struct virtio_buf){ dma_phys + 16, 1, true };
    nreq++;
    int head = virtq_submit(&vq, bufs, n);
    if (head < 0) {
        nerr++;
        return -1;
    }
    virtq_kick(&vq);
    uint64_t deadline = time_ms() + TIMEOUT_MS;
    for (;;) {
        int done = virtq_reap(&vq, NULL);
        if (done == head)
            break;
        if (time_ms() > deadline) {
            klog("vblk: request timed out (type %u sector %llu)", type, (unsigned long long)sector);
            nerr++;
            /* The chain stays owned by the device and could still be written
             * later, so the disk is taken offline instead of being reused. */
            present = false;
            return -1;
        }
        cpu_pause();
    }
    if (dma[16] != 0) {
        nerr++;
        return -1;
    }
    return 0;
}

int vblk_read(uint64_t sector, uint32_t count, void *buf)
{
    if (!present || count == 0 || count > MAX_SECTORS || sector + count > sectors)
        return -1;
    int r = do_request(VIRTIO_BLK_T_IN, sector, count, false);
    if (r == 0)
        memcpy(buf, dma + 4096, count * 512u);
    return r;
}

int vblk_write(uint64_t sector, uint32_t count, const void *buf)
{
    if (!present || vblk_readonly() || count == 0 || count > MAX_SECTORS || sector + count > sectors)
        return -1;
    memcpy(dma + 4096, buf, count * 512u);
    return do_request(VIRTIO_BLK_T_OUT, sector, count, true);
}

static int bd_read(void *ctx, uint64_t block, void *buf)
{
    UNUSED(ctx);
    return vblk_read(block * 8, 8, buf) ? PFS_E_IO : 0;
}

static int bd_write(void *ctx, uint64_t block, const void *buf)
{
    UNUSED(ctx);
    return vblk_write(block * 8, 8, buf) ? PFS_E_IO : 0;
}

static int bd_flush(void *ctx)
{
    UNUSED(ctx);
    return vblk_flush() ? PFS_E_IO : 0;
}

static struct pfs_blockdev bdev;

struct pfs_blockdev *vblk_blockdev(void)
{
    if (!present)
        return NULL;
    bdev = (struct pfs_blockdev){ NULL, sectors / 8, bd_read, bd_write, bd_flush };
    return &bdev;
}

int vblk_flush(void)
{
    if (!present)
        return -1;
    if (!(dev.features & VIRTIO_BLK_F_FLUSH))
        return 0; /* device has no volatile write cache to flush */
    return do_request(VIRTIO_BLK_T_FLUSH, 0, 0, false);
}
