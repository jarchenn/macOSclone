/* Temporary: no network stack yet. Replaced by the real stack in 0.4. */
#include "net/net.h"
#include "abi/aurora_abi.h"

static struct net_config cfg;
void net_init(void) {}
const struct net_config *net_info(void) { return &cfg; }
int net_dhcp(uint64_t t) { (void)t; return AE_NETDOWN; }
int net_ping(uint32_t ip, uint16_t seq, uint64_t t, uint64_t *rtt) { (void)ip; (void)seq; (void)t; (void)rtt; return AE_NETDOWN; }
int net_resolve(const char *n, uint32_t *ip, uint64_t t) { (void)n; (void)ip; (void)t; return AE_NETDOWN; }
int net_resolve_via(const char *n, uint32_t s, uint16_t p, uint32_t *ip, uint64_t t) { (void)n; (void)s; (void)p; (void)ip; (void)t; return AE_NETDOWN; }
bool net_parse_ip(const char *s, uint32_t *ip) { (void)s; (void)ip; return false; }
void net_format_ip(uint32_t ip, char *b, size_t c) { (void)ip; if (c) b[0] = 0; }
int sock_open(uint32_t t) { (void)t; return AE_NETDOWN; }
int sock_connect(int s, uint32_t ip, uint16_t p, uint64_t t) { (void)s; (void)ip; (void)p; (void)t; return AE_NETDOWN; }
long sock_send(int s, const void *b, uint64_t l) { (void)s; (void)b; (void)l; return AE_NETDOWN; }
long sock_recv(int s, void *b, uint64_t l, uint64_t t) { (void)s; (void)b; (void)l; (void)t; return AE_NETDOWN; }
int sock_bind(int s, uint16_t p) { (void)s; (void)p; return AE_NETDOWN; }
long sock_sendto(int s, const void *b, uint64_t l, uint32_t ip, uint16_t p) { (void)s; (void)b; (void)l; (void)ip; (void)p; return AE_NETDOWN; }
long sock_recvfrom(int s, void *b, uint64_t l, uint32_t *ip, uint16_t *p, uint64_t t) { (void)s; (void)b; (void)l; (void)ip; (void)p; (void)t; return AE_NETDOWN; }
int sock_close(int s) { (void)s; return 0; }
int sock_count(void) { return 0; }
const char *net_strerror(int e) { (void)e; return "network unavailable"; }
