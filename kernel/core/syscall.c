/* System call dispatch (int 0x80). Every pointer from user space is checked
 * against the calling process's page tables before it is touched. */
#include "core/process.h"
#include "core/sched.h"
#include "core/spinlock.h"
#include "core/time.h"
#include "core/log.h"
#include "arch/cpu.h"
#include "arch/x86_64/io.h"
#include "fs/vfs.h"
#include "net/net.h"
#include "kalloc.h"
#include "lib.h"

#define IO_CHUNK 4096

static int fd_alloc(struct process *p)
{
    for (int i = 3; i < PROC_MAX_FDS; i++)
        if (p->fds[i].kind == FD_NONE)
            return i;
    return AE_BADF;
}

static struct proc_fd *fd_get(struct process *p, uint64_t fd)
{
    if (fd >= PROC_MAX_FDS || p->fds[fd].kind == FD_NONE)
        return NULL;
    return &p->fds[fd];
}

static long sys_write(struct process *p, uint64_t fd, uint64_t buf, uint64_t len)
{
    struct proc_fd *d = fd_get(p, fd);
    if (!d)
        return AE_BADF;
    if (!user_range_ok(p, buf, len, false))
        return AE_FAULT;
    if (d->kind == FD_CONSOLE) {
        if (fd == 0)
            return AE_BADF;
        if (p->con.output)
            p->con.output(p->con.ctx, p->pid, (const char *)(uintptr_t)buf, len);
        sched_wake(sched_desktop());
        return (long)len;
    }
    if (d->kind == FD_SOCKET)
        return sock_send(d->handle, (const void *)(uintptr_t)buf, len);
    /* Files: bounce through a kernel buffer so the VFS never sees user pointers. */
    char *tmp = kmalloc(IO_CHUNK);
    if (!tmp)
        return AE_NOMEM;
    uint64_t done = 0;
    long r = 0;
    while (done < len) {
        uint64_t n = MIN(len - done, (uint64_t)IO_CHUNK);
        memcpy(tmp, (const void *)(uintptr_t)(buf + done), n);
        r = vfs_write(d->handle, tmp, n);
        if (r < 0)
            break;
        done += (uint64_t)r;
    }
    kfree(tmp);
    return done ? (long)done : r;
}

static long read_console(struct process *p, uint64_t buf, uint64_t len)
{
    if (!user_range_ok(p, buf, len, true))
        return AE_FAULT;
    for (;;) {
        uint64_t f = irq_save();
        if (p->in_r != p->in_w) {
            uint64_t n = 0;
            char *dst = (char *)(uintptr_t)buf;
            while (n < len && p->in_r != p->in_w) {
                char c = p->in[p->in_r++ % PROC_IN_CAP];
                dst[n++] = c;
                if (c == '\n')
                    break; /* line-at-a-time like a terminal */
            }
            irq_restore(f);
            return (long)n;
        }
        if (p->in_eof) {
            irq_restore(f);
            return 0;
        }
        if (p->kill_pending) {
            irq_restore(f);
            return AE_INTR;
        }
        sched_wait_on(&p->in, 0);
        irq_restore(f);
    }
}

static long sys_read(struct process *p, uint64_t fd, uint64_t buf, uint64_t len)
{
    struct proc_fd *d = fd_get(p, fd);
    if (!d)
        return AE_BADF;
    if (d->kind == FD_CONSOLE) {
        if (fd != 0)
            return AE_BADF;
        return read_console(p, buf, len);
    }
    if (!user_range_ok(p, buf, len, true))
        return AE_FAULT;
    if (d->kind == FD_SOCKET)
        return sock_recv(d->handle, (void *)(uintptr_t)buf, len, 0);
    char *tmp = kmalloc(IO_CHUNK);
    if (!tmp)
        return AE_NOMEM;
    uint64_t n = MIN(len, (uint64_t)IO_CHUNK);
    long r = vfs_read(d->handle, tmp, n);
    if (r > 0)
        memcpy((void *)(uintptr_t)buf, tmp, (size_t)r);
    kfree(tmp);
    return r;
}

static long sys_open(struct process *p, uint64_t upath, uint64_t flags)
{
    char path[VFS_PATH_MAX + 1];
    int r = copy_string_from_user(p, upath, path, sizeof path);
    if (r < 0)
        return r;
    int slot = fd_alloc(p);
    if (slot < 0)
        return slot;
    unsigned vf = 0;
    if (flags & AURORA_O_READ) vf |= VFS_O_READ;
    if (flags & AURORA_O_WRITE) vf |= VFS_O_WRITE;
    if (flags & AURORA_O_CREATE) vf |= VFS_O_CREATE;
    if (flags & AURORA_O_TRUNC) vf |= VFS_O_TRUNC;
    int h = vfs_open(path, vf);
    if (h < 0)
        return h;
    p->fds[slot].kind = FD_FILE;
    p->fds[slot].handle = h;
    return slot;
}

static long sys_close(struct process *p, uint64_t fd)
{
    struct proc_fd *d = fd_get(p, fd);
    if (!d || fd < 3)
        return AE_BADF;
    if (d->kind == FD_FILE)
        vfs_close(d->handle);
    else if (d->kind == FD_SOCKET)
        sock_close(d->handle);
    d->kind = FD_NONE;
    return 0;
}

static long sys_spawn(struct process *p, uint64_t upath, uint64_t uargs)
{
    char path[VFS_PATH_MAX + 1], args[AURORA_USER_ARGS_MAX];
    int r = copy_string_from_user(p, upath, path, sizeof path);
    if (r < 0)
        return r;
    args[0] = '\0';
    if (uargs) {
        r = copy_string_from_user(p, uargs, args, sizeof args);
        if (r < 0)
            return r;
    }
    return process_spawn(path, args, &p->con, p->pid);
}

static long sys_readdir(struct process *p, uint64_t upath, uint64_t index, uint64_t uout)
{
    char path[VFS_PATH_MAX + 1];
    int r = copy_string_from_user(p, upath, path, sizeof path);
    if (r < 0)
        return r;
    if (!user_range_ok(p, uout, sizeof(struct aurora_dirent), true))
        return AE_FAULT;
    struct vfs_dirent d;
    r = vfs_readdir(path, (uint32_t)index, &d);
    if (r)
        return r;
    struct aurora_dirent out;
    memset(&out, 0, sizeof out);
    str_copy(out.name, d.name, sizeof out.name);
    out.type = d.type;
    out.size = d.size;
    out.readonly = d.readonly;
    return copy_to_user(p, uout, &out, sizeof out);
}

static long sys_stat(struct process *p, uint64_t upath, uint64_t uout)
{
    char path[VFS_PATH_MAX + 1];
    int r = copy_string_from_user(p, upath, path, sizeof path);
    if (r < 0)
        return r;
    struct vfs_stat st;
    r = vfs_stat(path, &st);
    if (r)
        return r;
    struct aurora_stat out = { st.type, st.size, st.readonly, 0 };
    return copy_to_user(p, uout, &out, sizeof out);
}

static long sys_path_op(struct process *p, uint64_t upath, int (*op)(const char *))
{
    char path[VFS_PATH_MAX + 1];
    int r = copy_string_from_user(p, upath, path, sizeof path);
    if (r < 0)
        return r;
    return op(path);
}

static long sys_socket(struct process *p, uint64_t type)
{
    int slot = fd_alloc(p);
    if (slot < 0)
        return slot;
    int s = sock_open((uint32_t)type);
    if (s < 0)
        return s;
    p->fds[slot].kind = FD_SOCKET;
    p->fds[slot].handle = s;
    return slot;
}

static int sock_of(struct process *p, uint64_t fd)
{
    struct proc_fd *d = fd_get(p, fd);
    return (d && d->kind == FD_SOCKET) ? d->handle : AE_BADF;
}

static long sys_resolve(struct process *p, uint64_t uname, uint64_t uip, uint64_t timeout, uint64_t server, uint64_t port)
{
    char name[128];
    int r = copy_string_from_user(p, uname, name, sizeof name);
    if (r < 0)
        return r;
    if (!user_range_ok(p, uip, 4, true))
        return AE_FAULT;
    uint32_t ip = 0;
    if (server)
        r = net_resolve_via(name, (uint32_t)server, port ? (uint16_t)port : 53, &ip, timeout ? timeout : 3000);
    else
        r = net_resolve(name, &ip, timeout ? timeout : 3000);
    if (r)
        return r;
    return copy_to_user(p, uip, &ip, 4);
}

void syscall_dispatch(struct interrupt_frame *f)
{
    struct process *p = process_current();
    if (!p) {
        f->rax = (uint64_t)(int64_t)AE_NOSYS;
        return;
    }
    p->syscalls++;
    cpu_sti(); /* kernel code is not preempted, but IRQs keep flowing */
    bkl_lock(); /* system calls run under the big kernel lock (0.5) */
    uint64_t a = f->rdi, b = f->rsi, c = f->rdx, d = f->r10, e = f->r8;
    long r;
    switch (f->rax) {
    case SYS_EXIT:
        process_exit((int)a, "");
    case SYS_WRITE: r = sys_write(p, a, b, c); break;
    case SYS_READ: r = sys_read(p, a, b, c); break;
    case SYS_OPEN: r = sys_open(p, a, b); break;
    case SYS_CLOSE: r = sys_close(p, a); break;
    case SYS_GETPID: r = p->pid; break;
    case SYS_SLEEP: sched_sleep_ms(MIN(a, 3600000ull)); r = p->kill_pending ? AE_INTR : 0; break;
    case SYS_YIELD: sched_yield(); r = 0; break;
    case SYS_UPTIME: r = (long)time_ms(); break;
    case SYS_SPAWN: r = sys_spawn(p, a, b); break;
    case SYS_WAIT: r = process_wait(p, (int)a); break;
    case SYS_READDIR: r = sys_readdir(p, a, b, c); break;
    case SYS_STAT: r = sys_stat(p, a, b); break;
    case SYS_MKDIR: r = sys_path_op(p, a, vfs_mkdir); break;
    case SYS_UNLINK: r = sys_path_op(p, a, vfs_remove); break;
    case SYS_SYNC: r = vfs_sync(); break;
    case SYS_SOCKET: r = sys_socket(p, a); break;
    case SYS_CONNECT: {
        int s = sock_of(p, a);
        r = s < 0 ? s : sock_connect(s, (uint32_t)b, (uint16_t)c, d ? d : 5000);
        break;
    }
    case SYS_SEND: {
        int s = sock_of(p, a);
        r = s < 0 ? s : (user_range_ok(p, b, c, false) ? sock_send(s, (const void *)(uintptr_t)b, c) : AE_FAULT);
        break;
    }
    case SYS_RECV: {
        int s = sock_of(p, a);
        r = s < 0 ? s : (user_range_ok(p, b, c, true) ? sock_recv(s, (void *)(uintptr_t)b, c, d) : AE_FAULT);
        break;
    }
    case SYS_SOCKCLOSE: r = sys_close(p, a); break;
    case SYS_RESOLVE: r = sys_resolve(p, a, b, c, d, e); break;
    case SYS_BIND: {
        int s = sock_of(p, a);
        r = s < 0 ? s : sock_bind(s, (uint16_t)b);
        break;
    }
    case SYS_SENDTO: {
        int s = sock_of(p, a);
        r = s < 0 ? s : (user_range_ok(p, b, c, false) ? sock_sendto(s, (const void *)(uintptr_t)b, c, (uint32_t)d, (uint16_t)e) : AE_FAULT);
        break;
    }
    case SYS_RECVFROM: {
        int s = sock_of(p, a);
        if (s < 0) {
            r = s;
        } else if (!user_range_ok(p, b, c, true) || (d && !user_range_ok(p, d, sizeof(struct aurora_sockaddr), true))) {
            r = AE_FAULT;
        } else {
            struct aurora_sockaddr from = { 0, 0, 0 };
            r = sock_recvfrom(s, (void *)(uintptr_t)b, c, &from.ipv4_be, &from.port, e);
            if (r >= 0 && d)
                memcpy((void *)(uintptr_t)d, &from, sizeof from);
        }
        break;
    }
    case SYS_NETINFO: {
        const struct net_config *nc = net_info();
        struct aurora_netinfo ni;
        memset(&ni, 0, sizeof ni);
        ni.up = nc->up;
        ni.configured = nc->configured;
        memcpy(ni.mac, nc->mac, 6);
        ni.ip = nc->ip;
        ni.netmask = nc->netmask;
        ni.gateway = nc->gateway;
        ni.dns = nc->dns;
        ni.rx_packets = nc->rx_packets;
        ni.tx_packets = nc->tx_packets;
        r = copy_to_user(p, a, &ni, sizeof ni);
        break;
    }
    case SYS_PING: {
        uint64_t rtt = 0;
        r = net_ping((uint32_t)a, (uint16_t)b, c ? MIN(c, 30000ull) : 2000, &rtt);
        if (r == 0)
            r = (long)rtt;
        break;
    }
    default: r = AE_NOSYS; break;
    }
    if (p->kill_pending && r >= 0)
        r = AE_INTR;
    f->rax = (uint64_t)(int64_t)r;
    bkl_unlock();
}
