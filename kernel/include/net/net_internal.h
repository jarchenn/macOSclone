#ifndef AURORA_NET_INTERNAL_H
#define AURORA_NET_INTERNAL_H
#include "net/net.h"
#include "net/proto.h"
#include "net/netdev.h"

#define ETH_TYPE_IP 0x0800
#define ETH_TYPE_ARP 0x0806
#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP 6
#define IP_PROTO_UDP 17
#define IP_BROADCAST 0xFFFFFFFFu

extern struct net_config g_net;

/* Sends an IPv4 packet. Returns 0, or AE_HOSTUNREACH when the next hop's MAC
 * is unknown (an ARP request is sent; callers in task context use
 * arp_resolve() first). Never blocks. */
int ip_send(uint32_t dst, uint8_t proto, const void *payload, size_t len);
/* Blocks (task context) until the next hop for dst is in the ARP cache. */
int arp_resolve(uint32_t dst, uint64_t timeout_ms);

/* TCP (net/tcp.c) */
struct tcb;
void tcp_input(uint32_t src, uint32_t dst, const uint8_t *seg, size_t len);
void tcp_timers(uint64_t now_ms);
struct tcb *tcp_open(void);
int tcp_connect(struct tcb *t, uint32_t ip, uint16_t port, uint64_t timeout_ms);
long tcp_send(struct tcb *t, const void *buf, uint64_t len);
long tcp_recv(struct tcb *t, void *buf, uint64_t len, uint64_t timeout_ms);
void tcp_close(struct tcb *t);
int tcp_active_count(void);
const char *tcp_state_name(const struct tcb *t);
void tcp_describe(const struct tcb *t, char *buf, size_t cap);

#endif
