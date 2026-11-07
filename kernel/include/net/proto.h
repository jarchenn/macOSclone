#ifndef AURORA_NET_PROTO_H
#define AURORA_NET_PROTO_H
#include "types.h"

/* Portable protocol helpers (host-tested): byte order, Internet checksums,
 * IPv4 text forms, DNS and DHCP message encoding/decoding, TCP sequence
 * arithmetic. IPv4 addresses are kept in network byte order. */

static inline uint16_t bswap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static inline uint32_t bswap32(uint32_t v) { return __builtin_bswap32(v); }
#define htons(x) bswap16((uint16_t)(x))
#define ntohs(x) bswap16((uint16_t)(x))
#define htonl(x) bswap32((uint32_t)(x))
#define ntohl(x) bswap32((uint32_t)(x))

#define IPV4(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

uint32_t csum_add(uint32_t sum, const void *data, size_t len);
uint16_t csum_fold(uint32_t sum);
uint16_t inet_checksum(const void *data, size_t len);
/* TCP/UDP checksum over the IPv4 pseudo-header and the segment. */
uint16_t l4_checksum(uint32_t src, uint32_t dst, uint8_t proto, const void *seg, size_t len);

bool ip_parse(const char *s, uint32_t *ip);
void ip_format(uint32_t ip, char *buf, size_t cap);

/* DNS */
#define DNS_MAX_NAME 253
int dns_build_query(uint8_t *buf, size_t cap, uint16_t id, const char *name);
enum dns_result { DNS_OK = 0, DNS_E_MALFORMED = -1, DNS_E_NXDOMAIN = -2, DNS_E_NOADDR = -3, DNS_E_MISMATCH = -4, DNS_E_SERVFAIL = -5 };
int dns_parse_a(const uint8_t *msg, size_t len, uint16_t id, uint32_t *ip);

/* DHCP (RFC 2131) */
#define DHCP_DISCOVER 1
#define DHCP_OFFER 2
#define DHCP_REQUEST 3
#define DHCP_ACK 5
#define DHCP_NAK 6
struct dhcp_info {
    uint8_t type;
    uint32_t yiaddr, server_id, netmask, router, dns, lease_s;
};
size_t dhcp_build(uint8_t *buf, size_t cap, uint8_t type, uint32_t xid, const uint8_t mac[6],
                  uint32_t requested_ip, uint32_t server_id);
int dhcp_parse(const uint8_t *pkt, size_t len, uint32_t xid, const uint8_t mac[6], struct dhcp_info *out);

/* TCP sequence space comparisons (modulo 2^32). */
static inline bool seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
static inline bool seq_le(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }
static inline bool seq_gt(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }
static inline bool seq_ge(uint32_t a, uint32_t b) { return (int32_t)(a - b) >= 0; }

#endif
