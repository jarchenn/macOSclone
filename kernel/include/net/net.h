#ifndef AURORA_NET_H
#define AURORA_NET_H
#include "types.h"

/* Network stack public interface. Addresses are IPv4 in network byte order
 * ("_be"); ports are host order. All blocking calls take a timeout and run
 * in a task context (never from IRQ handlers). */

struct net_config {
    bool up;          /* driver present and link initialised */
    bool configured;  /* address assigned (DHCP or static) */
    uint8_t mac[6];
    uint32_t ip, netmask, gateway, dns; /* network byte order */
    uint64_t rx_packets, tx_packets, rx_dropped, tx_errors;
    char driver[24];
};

void net_init(void);                 /* probe NIC, start the network task */
const struct net_config *net_info(void);
int net_dhcp(uint64_t timeout_ms);    /* (re)acquire an address */
int net_ping(uint32_t ip, uint16_t seq, uint64_t timeout_ms, uint64_t *rtt_ms);
int net_resolve(const char *name, uint32_t *ip, uint64_t timeout_ms);
int net_resolve_via(const char *name, uint32_t server, uint16_t port, uint32_t *ip, uint64_t timeout_ms);
bool net_parse_ip(const char *s, uint32_t *ip);
void net_format_ip(uint32_t ip, char *buf, size_t cap);

/* Sockets (TCP and UDP). Handles are small integers >= 0. */
int sock_open(uint32_t type);
int sock_connect(int s, uint32_t ip, uint16_t port, uint64_t timeout_ms);
long sock_send(int s, const void *buf, uint64_t len);
long sock_recv(int s, void *buf, uint64_t len, uint64_t timeout_ms);
int sock_bind(int s, uint16_t port);
long sock_sendto(int s, const void *buf, uint64_t len, uint32_t ip, uint16_t port);
long sock_recvfrom(int s, void *buf, uint64_t len, uint32_t *ip, uint16_t *port, uint64_t timeout_ms);
int sock_close(int s);
int sock_count(void);
const char *net_strerror(int err);

#endif
