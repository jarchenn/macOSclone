/* virtio-net over legacy PCI: queue 0 receives, queue 1 transmits. Each
 * buffer is one descriptor holding the 10-byte virtio_net_hdr followed by
 * the Ethernet frame (no mergeable buffers, no offloads negotiated). */
#include "net/netdev.h"
#include "drivers/virtio.h"
#include "arch/x86_64/io.h"
#include "core/log.h"
#include "lib.h"

#define VIRTIO_NET_F_MAC (1u << 5)
#define HDR_LEN 10
#define BUF_SIZE 2048
#define RX_BUFS 64
#define TX_BUFS 32

static struct virtio_dev dev;
static struct virtq rxq, txq;
static uint8_t *rxmem, *txmem;
static phys_addr_t rx_phys, tx_phys;
static int16_t rx_head_buf[512]; /* descriptor head -> buffer index */
static int16_t tx_head_buf[512];
static bool tx_busy[TX_BUFS];
static bool present;

bool vnet_present(void) { return present; }

static void post_rx(int i)
{
    struct virtio_buf b = { rx_phys + (phys_addr_t)i * BUF_SIZE, BUF_SIZE, true };
    int head = virtq_submit(&rxq, &b, 1);
    if (head >= 0)
        rx_head_buf[head] = (int16_t)i;
}

bool vnet_init(uint8_t mac[6])
{
    const struct pci_dev *p = pci_find(0x1AF4, 0x1000);
    if (!p)
        return false;
    if (!virtio_begin(&dev, p, VIRTIO_NET_F_MAC))
        return false;
    if (!virtq_setup(&dev, &rxq, 0, 256) || !virtq_setup(&dev, &txq, 1, 256)) {
        outb(dev.io + VIRTIO_REG_STATUS, VIRTIO_STATUS_FAILED);
        return false;
    }
    rxmem = dma_alloc(RX_BUFS * BUF_SIZE / 4096, &rx_phys);
    txmem = dma_alloc(TX_BUFS * BUF_SIZE / 4096, &tx_phys);
    if (!rxmem || !txmem) {
        outb(dev.io + VIRTIO_REG_STATUS, VIRTIO_STATUS_FAILED);
        return false;
    }
    if (dev.features & VIRTIO_NET_F_MAC) {
        for (int i = 0; i < 6; i++)
            mac[i] = inb((uint16_t)(dev.io + VIRTIO_REG_CONFIG + i));
    } else {
        static const uint8_t fallback[6] = { 0x52, 0x54, 0x00, 0xA0, 0x00, 0x01 };
        memcpy(mac, fallback, 6);
    }
    virtio_finish(&dev);
    for (int i = 0; i < RX_BUFS; i++)
        post_rx(i);
    virtq_kick(&rxq);
    present = true;
    return true;
}

static void reap_tx(void)
{
    int head;
    while ((head = virtq_reap(&txq, NULL)) >= 0) {
        int i = tx_head_buf[head];
        if (i >= 0 && i < TX_BUFS)
            tx_busy[i] = false;
    }
}

int vnet_send(const void *frame, size_t len)
{
    if (!present || len > ETH_FRAME_MAX || len < 14)
        return -1;
    reap_tx();
    int slot = -1;
    for (int i = 0; i < TX_BUFS; i++)
        if (!tx_busy[i]) {
            slot = i;
            break;
        }
    if (slot < 0)
        return -1; /* all buffers in flight: caller retries or drops */
    uint8_t *b = txmem + (size_t)slot * BUF_SIZE;
    memset(b, 0, HDR_LEN);
    memcpy(b + HDR_LEN, frame, len);
    size_t total = HDR_LEN + (len < 60 ? 60 : len); /* pad runt frames */
    if (len < 60)
        memset(b + HDR_LEN + len, 0, 60 - len);
    struct virtio_buf vb = { tx_phys + (phys_addr_t)slot * BUF_SIZE, (uint32_t)total, false };
    int head = virtq_submit(&txq, &vb, 1);
    if (head < 0)
        return -1;
    tx_head_buf[head] = (int16_t)slot;
    tx_busy[slot] = true;
    virtq_kick(&txq);
    return 0;
}

int vnet_poll(void (*cb)(const uint8_t *frame, size_t len))
{
    if (!present)
        return 0;
    int n = 0;
    uint32_t len;
    int head;
    while ((head = virtq_reap(&rxq, &len)) >= 0) {
        int i = rx_head_buf[head];
        if (i >= 0 && i < RX_BUFS) {
            if (len > HDR_LEN && len <= BUF_SIZE)
                cb(rxmem + (size_t)i * BUF_SIZE + HDR_LEN, len - HDR_LEN);
            post_rx(i);
            n++;
        }
    }
    if (n)
        virtq_kick(&rxq);
    reap_tx();
    return n;
}
