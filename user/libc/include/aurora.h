/* Aurora-specific interfaces for user programs (0.6): the 0.4 socket API,
 * system information and small helpers. Everything POSIX comes from the
 * normal headers (newlib + Aurora's libc layer). */
#ifndef AURORA_H
#define AURORA_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <aurora_abi.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Sockets live in the descriptor table (read/write/close/poll work on
 * them); these calls cover connection set-up and datagrams. Timeouts are in
 * milliseconds (0 = the call's default). Errors: -1 with errno. */
int aurora_socket(uint32_t type); /* AURORA_SOCK_TCP / AURORA_SOCK_UDP */
int aurora_connect(int fd, uint32_t ip_be, uint16_t port, uint64_t timeout_ms);
long aurora_send(int fd, const void *buf, size_t len);
long aurora_recv(int fd, void *buf, size_t len, uint64_t timeout_ms);
int aurora_resolve(const char *name, uint32_t *ip_be, uint64_t timeout_ms);
int aurora_resolve_via(const char *name, uint32_t *ip_be, uint64_t timeout_ms, uint32_t server_be, uint16_t port);
int aurora_bind(int fd, uint16_t port);
long aurora_sendto(int fd, const void *buf, size_t len, uint32_t ip_be, uint16_t port);
long aurora_recvfrom(int fd, void *buf, size_t len, struct aurora_sockaddr *from, uint64_t timeout_ms);
int aurora_netinfo(struct aurora_netinfo *out);
long aurora_ping(uint32_t ip_be, uint16_t seq, uint64_t timeout_ms); /* round trip ms */
bool aurora_parse_ipv4(const char *s, uint32_t *ip_be);
void aurora_format_ipv4(uint32_t ip_be, char *buf, size_t cap);

uint64_t aurora_uptime_ms(void);
int aurora_sysinfo(struct aurora_sysinfo *out);
uint32_t crc32_update(uint32_t crc, const void *data, size_t len);

#ifdef __cplusplus
}
#endif
#endif
