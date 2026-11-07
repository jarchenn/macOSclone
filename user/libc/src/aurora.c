/* Aurora-specific calls (include/aurora.h). */
#include "sys.h"
#include <aurora.h>
#include <stdio.h>
#include <stdarg.h>

int aurora_socket(uint32_t type) { return (int)__ret(__sys1(SYS_SOCKET, type)); }
int aurora_connect(int fd, uint32_t ip, uint16_t port, uint64_t t) { return (int)__ret(__sys4(SYS_CONNECT, fd, ip, port, t)); }
long aurora_send(int fd, const void *buf, size_t len) { return __ret(__sys3(SYS_SEND, fd, buf, len)); }
long aurora_recv(int fd, void *buf, size_t len, uint64_t t) { return __ret(__sys4(SYS_RECV, fd, buf, len, t)); }
int aurora_resolve(const char *name, uint32_t *ip, uint64_t t) { return (int)__ret(__sys5(SYS_RESOLVE, name, ip, t, 0, 0)); }
int aurora_resolve_via(const char *name, uint32_t *ip, uint64_t t, uint32_t server, uint16_t port)
{
    return (int)__ret(__sys5(SYS_RESOLVE, name, ip, t, server, port));
}
int aurora_bind(int fd, uint16_t port) { return (int)__ret(__sys2(SYS_BIND, fd, port)); }
long aurora_sendto(int fd, const void *buf, size_t len, uint32_t ip, uint16_t port)
{
    return __ret(__sys5(SYS_SENDTO, fd, buf, len, ip, port));
}
long aurora_recvfrom(int fd, void *buf, size_t len, struct aurora_sockaddr *from, uint64_t t)
{
    return __ret(__sys5(SYS_RECVFROM, fd, buf, len, from, t));
}
int aurora_netinfo(struct aurora_netinfo *out) { return (int)__ret(__sys1(SYS_NETINFO, out)); }
long aurora_ping(uint32_t ip, uint16_t seq, uint64_t t) { return __ret(__sys3(SYS_PING, ip, seq, t)); }
uint64_t aurora_uptime_ms(void) { return (uint64_t)__sys0(SYS_UPTIME); }
int aurora_sysinfo(struct aurora_sysinfo *out) { return (int)__ret(__sys1(SYS_SYSINFO, out)); }

int aurora_shm_create(size_t size, int flags) { return (int)__ret(__sys2(SYS_SHM_CREATE, size, flags)); }
int aurora_socketpair(int fds[2], int flags) { return (int)__ret(__sys2(SYS_SOCKETPAIR, fds, flags)); }
int aurora_local_listen(const char *name, int backlog, int flags)
{
    return (int)__ret(__sys3(SYS_LOCAL_LISTEN, name, backlog, flags));
}
int aurora_local_connect(const char *name, int flags) { return (int)__ret(__sys2(SYS_LOCAL_CONNECT, name, flags)); }
int aurora_accept(int fd, int flags) { return (int)__ret(__sys2(SYS_ACCEPT, fd, flags)); }

long aurora_sendmsg(int fd, const void *buf, size_t len, const int *fds, unsigned nfds, unsigned msg_flags)
{
    struct aurora_msg m = { (uint64_t)(uintptr_t)buf, len, (uint64_t)(uintptr_t)fds, nfds, msg_flags };
    return __ret(__sys2(SYS_SENDMSG, fd, &m));
}

long aurora_recvmsg(int fd, void *buf, size_t len, int *fds, unsigned *nfds, unsigned msg_flags, unsigned *out_flags)
{
    struct aurora_msg m = { (uint64_t)(uintptr_t)buf, len, (uint64_t)(uintptr_t)fds, nfds ? *nfds : 0, msg_flags };
    long r = __ret(__sys2(SYS_RECVMSG, fd, &m));
    if (nfds)
        *nfds = r >= 0 ? m.nfds : 0;
    if (out_flags)
        *out_flags = r >= 0 ? m.flags : 0;
    return r;
}

void aurora_test_event(const char *fmt, ...)
{
    char buf[300];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n >= (int)sizeof buf)
        n = (int)sizeof buf - 1;
    __sys2(SYS_TEST_EVENT, buf, n);
}

int aurora_power(int how) { return (int)__ret(__sys1(SYS_POWER, how)); }
int aurora_klog(struct aurora_klog *out) { return (int)__ret(__sys1(SYS_KLOG, out)); }
int aurora_cpuinfo(struct aurora_cpus *out) { return (int)__ret(__sys1(SYS_CPUINFO, out)); }
int aurora_proclist(struct aurora_procs *out) { return (int)__ret(__sys1(SYS_PROCLIST, out)); }
long aurora_testop(int op, long arg, void *out) { return __ret(__sys3(SYS_TESTOP, op, arg, out)); }
int aurora_spawn(const char *path, char *const argv[], char *const envp[], const struct aurora_spawn *attr)
{
    return (int)__ret(__sys4(SYS_SPAWN, path, argv, envp, attr));
}

bool aurora_parse_ipv4(const char *s, uint32_t *ip_be)
{
    uint32_t parts[4];
    for (int i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9')
            return false;
        uint32_t v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (uint32_t)(*s++ - '0');
            if (++digits > 3 || v > 255)
                return false;
        }
        parts[i] = v;
        if (i < 3 && *s++ != '.')
            return false;
    }
    if (*s)
        return false;
    *ip_be = parts[0] | parts[1] << 8 | parts[2] << 16 | parts[3] << 24;
    return true;
}

void aurora_format_ipv4(uint32_t ip, char *buf, size_t cap)
{
    snprintf(buf, cap, "%u.%u.%u.%u", ip & 0xff, (ip >> 8) & 0xff, (ip >> 16) & 0xff, ip >> 24);
}
