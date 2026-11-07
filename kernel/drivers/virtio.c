#include "drivers/virtio.h"
#include "arch/x86_64/io.h"
#include "memory/physical.h"
#include "core/boot.h"
#include "core/log.h"
#include "lib.h"

#define mb() __atomic_thread_fence(__ATOMIC_SEQ_CST)

void *dma_alloc(size_t pages, phys_addr_t *phys)
{
    phys_addr_t p = pmm_alloc_contig(pages);
    if (!p)
        return NULL;
    *phys = p;
    return phys_to_virt(p); /* the HHDM maps all usable RAM */
}

bool virtio_begin(struct virtio_dev *v, const struct pci_dev *pci, uint32_t wanted)
{
    v->pci = pci;
    v->io = pci_io_bar(pci, 0);
    if (!v->io)
        return false;
    pci_enable(pci);
    outb(v->io + VIRTIO_REG_STATUS, 0); /* reset */
    outb(v->io + VIRTIO_REG_STATUS, VIRTIO_STATUS_ACK);
    outb(v->io + VIRTIO_REG_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);
    uint32_t offered = inl(v->io + VIRTIO_REG_HOST_FEATURES);
    v->features = offered & wanted;
    outl(v->io + VIRTIO_REG_GUEST_FEATURES, v->features);
    return true;
}

void virtio_finish(struct virtio_dev *v)
{
    outb(v->io + VIRTIO_REG_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);
}

static size_t align4k(size_t x) { return (x + 4095) & ~(size_t)4095; }

bool virtq_setup(struct virtio_dev *v, struct virtq *q, uint16_t index, uint16_t max_num)
{
    UNUSED(max_num);
    outw(v->io + VIRTIO_REG_QUEUE_SEL, index);
    uint16_t num = inw(v->io + VIRTIO_REG_QUEUE_NUM); /* fixed by the device in legacy mode */
    if (num == 0 || (num & (num - 1)))
        return false;
    size_t desc_avail = 16u * num + 6u + 2u * num;
    size_t used = 6u + 8u * num;
    size_t bytes = align4k(desc_avail) + align4k(used);
    q->pages = bytes / 4096;
    uint8_t *mem = dma_alloc(q->pages, &q->phys);
    if (!mem)
        return false;
    q->num = num;
    q->index = index;
    q->io = v->io;
    q->desc = (struct vring_desc *)mem;
    q->avail = (volatile uint16_t *)(mem + 16u * num);
    q->used = (volatile uint16_t *)(mem + align4k(desc_avail));
    for (uint16_t i = 0; i < num; i++)
        q->desc[i].next = (uint16_t)(i + 1);
    q->free_head = 0;
    q->num_free = num;
    q->last_used = 0;
    outl(v->io + VIRTIO_REG_QUEUE_PFN, (uint32_t)(q->phys >> 12));
    return true;
}

int virtq_submit(struct virtq *q, const struct virtio_buf *bufs, int n)
{
    if (n <= 0 || q->num_free < n)
        return -1;
    uint16_t head = q->free_head, idx = head, prev = head;
    for (int i = 0; i < n; i++) {
        struct vring_desc *d = &q->desc[idx];
        d->addr = bufs[i].addr;
        d->len = bufs[i].len;
        d->flags = (uint16_t)((bufs[i].device_writes ? VRING_DESC_F_WRITE : 0) | (i + 1 < n ? VRING_DESC_F_NEXT : 0));
        prev = idx;
        idx = d->next;
    }
    q->free_head = idx;
    q->num_free = (uint16_t)(q->num_free - n);
    UNUSED(prev);
    uint16_t aidx = q->avail[1];
    q->avail[2 + (aidx % q->num)] = head;
    mb(); /* descriptors and ring slot before the index */
    q->avail[1] = (uint16_t)(aidx + 1);
    mb();
    return head;
}

void virtq_kick(struct virtq *q) { outw(q->io + VIRTIO_REG_QUEUE_NOTIFY, q->index); }

int virtq_reap(struct virtq *q, uint32_t *len)
{
    mb();
    uint16_t uidx = q->used[1];
    if (uidx == q->last_used)
        return -1;
    mb(); /* index before element */
    volatile struct vring_used_elem *ue =
        (volatile struct vring_used_elem *)((volatile uint8_t *)q->used + 4) + (q->last_used % q->num);
    uint32_t head = ue->id;
    if (len)
        *len = ue->len;
    q->last_used++;
    if (head >= q->num)
        return -1;
    /* Return the chain to the free list. */
    uint16_t idx = (uint16_t)head, count = 1;
    while (q->desc[idx].flags & VRING_DESC_F_NEXT) {
        idx = q->desc[idx].next;
        count++;
    }
    q->desc[idx].next = q->free_head;
    q->free_head = (uint16_t)head;
    q->num_free = (uint16_t)(q->num_free + count);
    return (int)head;
}
