#ifndef AURORA_BLK_H
#define AURORA_BLK_H
#include "types.h"

/* virtio-blk driver (legacy PCI). Requests are synchronous: submitted,
 * then polled for completion with a bounded timeout. */
bool vblk_init(void);
bool vblk_present(void);
uint64_t vblk_sectors(void);       /* 512-byte sectors */
bool vblk_readonly(void);
int vblk_read(uint64_t sector, uint32_t count, void *buf);
int vblk_write(uint64_t sector, uint32_t count, const void *buf);
int vblk_flush(void);
uint64_t vblk_requests(void);
uint64_t vblk_errors(void);
struct pfs_blockdev;
/* 4 KiB-block view of the disk for the persistent filesystem. */
struct pfs_blockdev *vblk_blockdev(void);

#endif
