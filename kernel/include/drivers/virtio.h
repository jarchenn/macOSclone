#ifndef AURORA_VIRTIO_H
#define AURORA_VIRTIO_H
#include "types.h"
#include "drivers/pci.h"

/* Legacy ("transitional") virtio over PCI I/O BAR0, as exposed by QEMU's
 * virtio-*-pci devices. Split virtqueues in physically contiguous memory.
 *
 * Ownership: a descriptor belongs to the device from the moment its head is
 * published in the avail ring until it appears in the used ring; the driver
 * never touches it in between. Memory barriers order descriptor writes before
 * the avail index update and the used index read before reading used
 * elements. */

#define VIRTIO_REG_HOST_FEATURES 0x00
#define VIRTIO_REG_GUEST_FEATURES 0x04
#define VIRTIO_REG_QUEUE_PFN 0x08
#define VIRTIO_REG_QUEUE_NUM 0x0C
#define VIRTIO_REG_QUEUE_SEL 0x0E
#define VIRTIO_REG_QUEUE_NOTIFY 0x10
#define VIRTIO_REG_STATUS 0x12
#define VIRTIO_REG_ISR 0x13
#define VIRTIO_REG_CONFIG 0x14

#define VIRTIO_STATUS_ACK 1
#define VIRTIO_STATUS_DRIVER 2
#define VIRTIO_STATUS_DRIVER_OK 4
#define VIRTIO_STATUS_FAILED 128

#define VRING_DESC_F_NEXT 1
#define VRING_DESC_F_WRITE 2

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct vring_used_elem {
    uint32_t id;
    uint32_t len;
};

struct virtq {
    uint16_t num;
    uint16_t index;
    uint16_t io;
    struct vring_desc *desc;
    volatile uint16_t *avail; /* flags, idx, ring[num], used_event */
    volatile uint16_t *used;  /* flags, idx, then used elements */
    uint16_t free_head;
    uint16_t num_free;
    uint16_t last_used;
    phys_addr_t phys;
    size_t pages;
};

struct virtio_buf {
    phys_addr_t addr;
    uint32_t len;
    bool device_writes;
};

struct virtio_dev {
    const struct pci_dev *pci;
    uint16_t io;
    uint32_t features;
};

bool virtio_begin(struct virtio_dev *v, const struct pci_dev *pci, uint32_t wanted_features);
void virtio_finish(struct virtio_dev *v);
bool virtq_setup(struct virtio_dev *v, struct virtq *q, uint16_t index, uint16_t max_num);
/* Publishes a descriptor chain; returns the head index or -1 if full. */
int virtq_submit(struct virtq *q, const struct virtio_buf *bufs, int n);
void virtq_kick(struct virtq *q);
/* Reaps one completed chain; returns its head or -1 if none is ready. */
int virtq_reap(struct virtq *q, uint32_t *len);

/* Physically contiguous, zeroed, page-aligned DMA memory. */
void *dma_alloc(size_t pages, phys_addr_t *phys);

#endif
