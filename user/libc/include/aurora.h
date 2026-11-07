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

/* 0.7: shared memory and local message sockets (see aurora_abi.h). flags:
 * O_CLOEXEC / O_NONBLOCK for the new descriptors. */
int aurora_shm_create(size_t size, int flags);
int aurora_socketpair(int fds[2], int flags);
int aurora_local_listen(const char *name, int backlog, int flags);
int aurora_local_connect(const char *name, int flags);
int aurora_accept(int fd, int flags);
/* msg_flags: AURORA_MSG_DONTWAIT. recvmsg: *nfds is the room in fds on
 * entry and the number received on return; *out_flags gets MSG_TRUNC. */
long aurora_sendmsg(int fd, const void *buf, size_t len, const int *fds, unsigned nfds, unsigned msg_flags);
long aurora_recvmsg(int fd, void *buf, size_t len, int *fds, unsigned *nfds, unsigned msg_flags, unsigned *out_flags);
/* Test builds: prints "@@<text>" on the serial log (no-op otherwise). */
void aurora_test_event(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int aurora_power(int how); /* 1 restart, 2 power off (root) */
int aurora_klog(struct aurora_klog *out);
int aurora_cpuinfo(struct aurora_cpus *out);
int aurora_proclist(struct aurora_procs *out);
long aurora_testop(int op, long arg, void *out); /* test builds, root */
/* Native process creation with an explicit descriptor map and flags
 * (AURORA_SPAWN_*): returns the pid or -1 with errno. */
int aurora_spawn(const char *path, char *const argv[], char *const envp[], const struct aurora_spawn *attr);
uint32_t crc32_update(uint32_t crc, const void *data, size_t len);

#ifdef __cplusplus
}
#endif
#endif
