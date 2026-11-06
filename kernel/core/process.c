/* Process creation, ELF loading, exit/kill/fault handling, user memory. */
#include "core/process.h"
#include "core/sched.h"
#include "core/elf.h"
#include "core/log.h"
#include "core/panic.h"
#include "core/time.h"
#include "arch/cpu.h"
#include "arch/paging.h"
#include "arch/x86_64/io.h"
#include "memory/physical.h"
#include "memory/virtual.h"
#include "memory/heap.h"
#include "fs/vfs.h"
#include "net/net.h"
#include "lib.h"

void enter_user(uint64_t rip, uint64_t rsp, uint64_t arg0, uint64_t arg1);

static struct process procs[MAX_PROCS];
static int next_pid = 1;

struct process *process_current(void)
{
    struct task *t = sched_current();
    return t ? t->proc : NULL;
}

struct process *process_get(int pid)
{
    for (int i = 0; i < MAX_PROCS; i++)
        if (procs[i].state != PROC_FREE && procs[i].pid == pid)
            return &procs[i];
    return NULL;
}

struct process *process_at(int i)
{
    return (i >= 0 && i < MAX_PROCS && procs[i].state != PROC_FREE) ? &procs[i] : NULL;
}

int process_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_PROCS; i++)
        n += procs[i].state == PROC_RUNNING;
    return n;
}

phys_addr_t process_space(const struct process *p) { return p->pml4_phys; }
bool process_kill_pending(const struct process *p) { return p && p->kill_pending; }

const char *process_spawn_error(int err)
{
    if (err == AE_NOEXEC)
        return "not a valid Aurora executable";
    if (err == AE_PROCLIM)
        return "too many processes";
    if (err == AE_NOMEM)
        return "out of memory";
    if (err == AE_INVAL)
        return "invalid arguments";
    return vfs_strerror(err);
}

/* ---- user memory --------------------------------------------------------- */

bool user_range_ok(struct process *p, uint64_t addr, uint64_t len, bool write)
{
    if (!p)
        return false;
    if (len == 0)
        return true;
    const uint64_t top = 0x0000800000000000ull;
    if (addr < PAGE_SIZE || addr >= top || len > top - addr || len > (64ull << 20))
        return false;
    for (uint64_t va = ALIGN_DOWN(addr, PAGE_SIZE); va < addr + len; va += PAGE_SIZE) {
        uint64_t pte = vmm_user_pte(p->pml4, va);
        if (!(pte & PTE_P) || !(pte & PTE_U) || (write && !(pte & PTE_W)))
            return false;
    }
    return true;
}

/* System calls run with the process's CR3 loaded, so validated user
 * addresses can be accessed directly. */
int copy_from_user(struct process *p, void *dst, uint64_t src, uint64_t len)
{
    if (!user_range_ok(p, src, len, false))
        return AE_FAULT;
    memcpy(dst, (const void *)(uintptr_t)src, len);
    return 0;
}

int copy_to_user(struct process *p, uint64_t dst, const void *src, uint64_t len)
{
    if (!user_range_ok(p, dst, len, true))
        return AE_FAULT;
    memcpy((void *)(uintptr_t)dst, src, len);
    return 0;
}

int copy_string_from_user(struct process *p, uint64_t src, char *dst, size_t cap)
{
    size_t n = 0;
    while (n + 1 < cap) {
        if ((n == 0 || ((src + n) & (PAGE_SIZE - 1)) == 0) && !user_range_ok(p, src + n, 1, false))
            return AE_FAULT;
        char c = *(const char *)(uintptr_t)(src + n);
        dst[n] = c;
        if (!c)
            return (int)n;
        n++;
    }
    dst[cap - 1] = '\0';
    return AE_INVAL; /* unterminated or too long */
}

/* ---- creation ------------------------------------------------------------- */

static bool map_segment(struct process *p, const uint8_t *file, const struct elf_segment *s)
{
    uint64_t start = ALIGN_DOWN(s->vaddr, PAGE_SIZE), end = ALIGN_UP(s->vaddr + s->memsz, PAGE_SIZE);
    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        phys_addr_t pa = pmm_alloc_page(); /* zeroed: covers .bss */
        if (!pa)
            return false;
        /* Copy the part of [vaddr, vaddr+filesz) that falls in this page. */
        uint64_t lo = MAX(va, s->vaddr), hi = MIN(va + PAGE_SIZE, s->vaddr + s->filesz);
        if (hi > lo)
            memcpy((uint8_t *)phys_to_virt(pa) + (lo - va), file + s->offset + (lo - s->vaddr), hi - lo);
        if (!vmm_map_user(p->pml4, va, pa, s->writable, s->executable)) {
            pmm_free_page(pa);
            return false;
        }
        p->user_pages++;
    }
    return true;
}

static void proc_entry(void *arg)
{
    struct process *p = arg;
    /* The argument string sits in the top 256 bytes of the stack. */
    uint64_t args_va = AURORA_USER_STACK_TOP - AURORA_USER_ARGS_MAX;
    uint64_t rsp = ALIGN_DOWN(args_va - 64, 16);
    enter_user(p->entry, rsp, args_va, strlen(p->args));
}

int process_spawn(const char *path, const char *args, const struct proc_console *con, int parent)
{
    char norm[VFS_PATH_MAX + 1];
    int r = vfs_normalize("/", path, norm, sizeof norm);
    if (r)
        return r;
    if (strlen(args ? args : "") >= AURORA_USER_ARGS_MAX)
        return AE_INVAL;
    struct vfs_stat st;
    r = vfs_stat(norm, &st);
    if (r)
        return r;
    if (st.type != VFS_FILE)
        return VFS_EISDIR;
    if (st.size > PROC_MAX_IMAGE)
        return AE_NOEXEC;
    struct process *p = NULL;
    for (int i = 0; i < MAX_PROCS; i++)
        if (procs[i].state == PROC_FREE) {
            p = &procs[i];
            break;
        }
    if (!p)
        return AE_PROCLIM;
    uint8_t *file = kmalloc(st.size ? st.size : 1);
    if (!file)
        return AE_NOMEM;
    size_t len = 0;
    r = vfs_read_file(norm, file, st.size, &len);
    struct elf_image img;
    if (r == VFS_OK) {
        int e = elf_validate(file, len, AURORA_USER_BASE, AURORA_USER_LIMIT, &img);
        if (e != ELF_OK) {
            klog("proc: %s rejected: %s", norm, elf_strerror(e));
            r = AE_NOEXEC;
        }
    }
    if (r) {
        kfree(file);
        return r;
    }
    memset(p, 0, sizeof *p);
    p->pml4 = vmm_space_create(&p->pml4_phys);
    if (!p->pml4) {
        kfree(file);
        return AE_NOMEM;
    }
    bool ok = true;
    for (int i = 0; i < img.nseg && ok; i++)
        ok = map_segment(p, file, &img.seg[i]);
    kfree(file);
    /* Stack, with the argument string in its top page. The page below the
     * stack stays unmapped as a guard. */
    for (uint64_t va = AURORA_USER_STACK_TOP - AURORA_USER_STACK_SIZE; ok && va < AURORA_USER_STACK_TOP; va += PAGE_SIZE) {
        phys_addr_t pa = pmm_alloc_page();
        ok = pa && vmm_map_user(p->pml4, va, pa, true, false);
        if (pa && !ok)
            pmm_free_page(pa);
        if (ok)
            p->user_pages++;
        if (ok && va == AURORA_USER_STACK_TOP - PAGE_SIZE)
            str_copy((char *)phys_to_virt(pa) + (PAGE_SIZE - AURORA_USER_ARGS_MAX), args ? args : "", AURORA_USER_ARGS_MAX);
    }
    if (!ok) {
        vmm_space_destroy(p->pml4, p->pml4_phys);
        memset(p, 0, sizeof *p);
        return AE_NOMEM;
    }
    p->pid = next_pid++;
    p->state = PROC_RUNNING;
    p->parent = parent;
    const char *base = strrchr(norm, '/');
    str_copy(p->name, base ? base + 1 : norm, sizeof p->name);
    str_copy(p->args, args ? args : "", sizeof p->args);
    if (con)
        p->con = *con;
    p->fds[0].kind = FD_CONSOLE;
    p->fds[1].kind = FD_CONSOLE;
    p->fds[2].kind = FD_CONSOLE;
    p->start_ms = time_ms();
    p->entry = img.entry;
    p->task = task_create(p->name, proc_entry, p, p);
    if (!p->task) {
        vmm_space_destroy(p->pml4, p->pml4_phys);
        memset(p, 0, sizeof *p);
        return AE_PROCLIM;
    }
    klog("proc: started %s (pid %d, %llu pages, entry %llx)", p->name, p->pid,
         (unsigned long long)p->user_pages, (unsigned long long)img.entry);
    TEST_EVENT("PROC_START pid=%d name=%s", p->pid, p->name);
    return p->pid;
}

/* ---- exit / kill / fault ---------------------------------------------- */

static void close_fds(struct process *p)
{
    for (int i = 0; i < PROC_MAX_FDS; i++) {
        if (p->fds[i].kind == FD_FILE)
            vfs_close(p->fds[i].handle);
        else if (p->fds[i].kind == FD_SOCKET)
            sock_close(p->fds[i].handle);
        p->fds[i].kind = FD_NONE;
    }
}

static void release(struct process *p)
{
    memset(p, 0, sizeof *p);
}

NORETURN void process_exit(int status, const char *reason)
{
    struct process *p = process_current();
    if (!p)
        panic("process_exit outside a process");
    close_fds(p);
    p->status = status;
    str_copy(p->reason, reason ? reason : "", sizeof p->reason);
    p->state = PROC_ZOMBIE;
    TEST_EVENT("PROC_EXIT pid=%d name=%s status=%d reason=%s", p->pid, p->name, status, p->reason);
    klog("proc: %s (pid %d) exited with status %d%s%s", p->name, p->pid, status, p->reason[0] ? ": " : "", p->reason);
    /* Programs started by another process report to their parent (wait);
     * only top-level programs get an exit banner on the console. */
    if (p->con.exited && (p->parent == 0 || status != 0 || p->reason[0]))
        p->con.exited(p->con.ctx, p->pid, p->name, status, p->reason);
    /* Leave the address space before freeing it. */
    write_cr3(vmm_kernel_space());
    vmm_space_destroy(p->pml4, p->pml4_phys);
    p->pml4 = NULL;
    p->pml4_phys = 0;
    /* Children of this process are re-parented to the kernel; zombies freed. */
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].state != PROC_FREE && procs[i].parent == p->pid) {
            if (procs[i].state == PROC_ZOMBIE)
                release(&procs[i]);
            else
                procs[i].parent = 0;
        }
    }
    struct task *t = sched_current();
    t->proc = NULL;
    if (p->parent == 0 || !process_get(p->parent))
        release(p); /* nobody will wait for it */
    else
        sched_wake_all(p);
    sched_wake(sched_desktop());
    task_exit();
}

void process_check_kill(void)
{
    struct process *p = process_current();
    if (p && p->kill_pending)
        process_exit(-1, "killed");
}

int process_kill(int pid)
{
    struct process *p = process_get(pid);
    if (!p || p->state != PROC_RUNNING)
        return AE_CHILD;
    p->kill_pending = true;
    sched_wake(p->task);
    sched_wake_all(&p->in);
    return 0;
}

NORETURN void process_fault(const struct interrupt_frame *f)
{
    static const char *names[32] = { "divide error", "debug", "NMI", "breakpoint", "overflow", "bound range",
        "invalid opcode", "device not available", "double fault", "", "invalid TSS", "segment not present",
        "stack fault", "general protection fault", "page fault", "", "x87 error", "alignment check",
        "machine check", "SIMD error" };
    char reason[96];
    uint64_t v = f->vector;
    if (v == 14) {
        uint64_t cr2 = read_cr2();
        k_snprintf(reason, sizeof reason, "segmentation fault: %s %s at %llx (rip %llx)",
                   (f->error_code & 2) ? "write" : ((f->error_code & 16) ? "exec" : "read"),
                   (f->error_code & 1) ? "denied" : "unmapped", (unsigned long long)cr2,
                   (unsigned long long)f->rip);
    } else {
        k_snprintf(reason, sizeof reason, "%s (rip %llx)", v < 20 && names[v][0] ? names[v] : "CPU exception",
                   (unsigned long long)f->rip);
    }
    cpu_sti();
    process_exit(128 + (int)v, reason);
}

int process_input(int pid, const char *data, size_t len, bool eof)
{
    struct process *p = process_get(pid);
    if (!p || p->state != PROC_RUNNING)
        return AE_CHILD;
    for (size_t i = 0; i < len; i++) {
        if (p->in_w - p->in_r >= PROC_IN_CAP)
            break; /* drop on overflow */
        p->in[p->in_w++ % PROC_IN_CAP] = data[i];
    }
    if (eof)
        p->in_eof = true;
    sched_wake_all(&p->in);
    return 0;
}

int process_wait(struct process *self, int child)
{
    struct process *c = process_get(child);
    if (!c || c->parent != self->pid)
        return AE_CHILD;
    for (;;) {
        if (c->state == PROC_ZOMBIE) {
            int s = c->status;
            release(c);
            return s;
        }
        if (self->kill_pending)
            return AE_INTR;
        uint64_t f = irq_save();
        if (c->state != PROC_ZOMBIE)
            sched_wait_on(c, 0);
        irq_restore(f);
    }
}
