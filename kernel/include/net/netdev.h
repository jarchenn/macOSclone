#ifndef AURORA_NETDEV_H
#define AURORA_NETDEV_H
#include "types.h"

/* virtio-net (legacy PCI) driver interface used by the network stack. */
#define NET_MTU 1500
#define ETH_FRAME_MAX 1514

bool vnet_init(uint8_t mac[6]);
bool vnet_present(void);
int vnet_send(const void *frame, size_t len);   /* 0 or -1 */
/* Delivers every completed receive buffer to cb; returns frames handled. */
int vnet_poll(void (*cb)(const uint8_t *frame, size_t len));

#endif
