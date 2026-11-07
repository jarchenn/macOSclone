/* Processes and threads (0.6): creation from ELF images (kernel start,
 * spawn), fork with copy-on-write, execve, threads, exit, wait, the process
 * tree and futexes. Signals live in core/signal.c. Everything runs under
 * the BKL. */
#include "core/process.h"
#include "core/file.h"
#include "core/pty.h"
#include "core/sched.h"
#include "core/spinlock.h"
#include "core/elf.h"
#include "core/log.h"
#include "core/panic.h"
#include "core/time.h"
#include "arch/cpu.h"
#include "arch/paging.h"
#include "arch/percpu.h"
#include "arch/x86_64/io.h"
#include "memory/physical.h"
#include "memory/virtual.h"
#include "kalloc.h"
#include "lib.h"

NORETURN void enter_user_frame(const struct interrupt_frame *f);

static struct process procs[MAX_PROCS];
static int next_pid = 1;
static int init_pid;
static uint64_t boot_epoch;

uint64_t process_boot_epoch(void) { return boot_epoch; }

struct process *process_current(void)
{
    struct task *t = sched_current();
    return t ? t->proc : NULL;
}

struct process *process_get(int pid)
{
    if (pid <= 0)
        return NULL;
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

int process_thread_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_PROCS; i++)
        if (procs[i].state == PROC_RUNNING)
            n += procs[i].nthreads;
    return n;
}

uint64_t process_cpu_ticks(const struct process *p)
{
    uint64_t t = p->dead_ticks;
    for (int i = 0; i < p->nthreads; i++)
        if (p->threads[i])
            t += p->threads[i]->cpu_ticks;
    return t;
}

phys_addr_t process_space(const struct process *p) { return p->mm.pml4_phys; }

const char *process_spawn_error(int err)
{
    switch (err) {
    case AE_NOEXEC: return "not a valid Aurora executable";
    case AE_PROCLIM: return "too many processes";
    case AE_NOMEM: return "out of memory";
    case AE_INVAL: return "invalid arguments";
    case AE_ACCES: return "permission denied";
    case AE_2BIG: return "argument list too long";
    default: return vfs_strerror(err);
    }
}

/* ---- user memory --------------------------------------------------------- */

bool user_range_ok(struct process *p, uint64_t addr, uint64_t len, bool write)
{
    return p && mm_prepare(&p->mm, addr, len, write);
}

/* System calls run with the process's CR3 loaded, so prepared user
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

/* Copies into an address space that may not be loaded (the loader). */
static bool copy_to_mm(struct mm *m, uint64_t va, const void *src, size_t len)
{
    const uint8_t *s = src;
    while (len) {
        uint8_t *d = mm_populate(m, va);
        if (!d)
            return false;
        size_t n = MIN(len, (size_t)(PAGE_SIZE - (va & (PAGE_SIZE - 1))));
        memcpy(d, s, n);
        va += n;
        s += n;
        len -= n;
    }
    return true;
}

/* ---- descriptors ------------------------------------------------------------ */

int proc_fd_install(struct process *p, struct file *f, int min, bool cloexec)
{
    for (int i = min < 0 ? 0 : min; i < PROC_MAX_FDS; i++) {
        if (!p->fds[i].f) {
            p->fds[i].f = f;
            p->fds[i].cloexec = cloexec;
            return i;
        }
    }
    return AE_MFILE;
}

struct file *proc_fd_get(struct process *p, int fd)
{
    return (fd >= 0 && fd < PROC_MAX_FDS) ? p->fds[fd].f : NULL;
}

int proc_fd_close(struct process *p, int fd)
{
    struct file *f = proc_fd_get(p, fd);
    if (!f)
        return AE_BADF;
    p->fds[fd].f = NULL;
    p->fds[fd].cloexec = false;
    file_put(f);
    return 0;
}

static void close_all(struct process *p)
{
    for (int i = 0; i < PROC_MAX_FDS; i++)
        if (p->fds[i].f)
            proc_fd_close(p, i);
}

/* ---- argument vectors ---------------------------------------------------- */

/* argv/envp gathered in one kernel block: strings then pointer tables. */
struct argblock {
    char *strings; /* argc + envc NUL-terminated strings, back to back */
    size_t bytes;
    int argc, envc;
};

static void argblock_free(struct argblock *a)
{
    if (a->strings)
        kfree(a->strings);
    memset(a, 0, sizeof *a);
}

static int argblock_add(struct argblock *a, const char *s, size_t len)
{
    if (a->bytes + len + 1 > AURORA_ARG_MAX)
        return AE_2BIG;
    memcpy(a->strings + a->bytes, s, len);
    a->strings[a->bytes + len] = '\0';
    a->bytes += len + 1;
    return 0;
}

static int argblock_from_kernel(struct argblock *a, const char *const *argv, const char *const *envp)
{
    memset(a, 0, sizeof *a);
    a->strings = kmalloc(AURORA_ARG_MAX);
    if (!a->strings)
        return AE_NOMEM;
    for (int pass = 0; pass < 2; pass++) {
        const char *const *v = pass ? envp : argv;
        for (int i = 0; v && v[i]; i++) {
            int r = argblock_add(a, v[i], strlen(v[i]));
            if (r) {
                argblock_free(a);
                return r;
            }
            if (pass)
                a->envc++;
            else
                a->argc++;
        }
    }
    return 0;
}

static int argblock_from_user(struct process *p, struct argblock *a, uint64_t uargv, uint64_t uenvp)
{
    memset(a, 0, sizeof *a);
    a->strings = kmalloc(AURORA_ARG_MAX);
    char *tmp = kmalloc(4096);
    if (!a->strings || !tmp) {
        if (tmp)
            kfree(tmp);
        argblock_free(a);
        return AE_NOMEM;
    }
    int r = 0;
    for (int pass = 0; pass < 2 && !r; pass++) {
        uint64_t vec = pass ? uenvp : uargv;
        if (!vec)
            continue;
        for (int i = 0; !r; i++) {
            if (i > 4096) {
                r = AE_2BIG;
                break;
            }
            uint64_t ptr;
            r = copy_from_user(p, &ptr, vec + (uint64_t)i * 8, 8);
            if (r || !ptr)
                break;
            int n = copy_string_from_user(p, ptr, tmp, 4096);
            if (n < 0) {
                r = n == AE_INVAL ? AE_2BIG : n;
                break;
            }
            r = argblock_add(a, tmp, (size_t)n);
            if (!r) {
                if (pass)
                    a->envc++;
                else
                    a->argc++;
            }
        }
    }
    kfree(tmp);
    if (r)
        argblock_free(a);
    return r;
}

static void args_summary(const struct argblock *a, char *out, size_t cap)
{
    out[0] = '\0';
    const char *s = a->strings;
    for (int i = 0; i < a->argc; i++) {
        if (i > 1)
            str_append(out, " ", cap);
        if (i > 0)
            str_append(out, s, cap);
        s += strlen(s) + 1;
    }
}

/* ---- ELF loading ------------------------------------------------------------- */

struct loaded {
    uint64_t entry, sp;
};

static bool exec_permitted(const struct process *p, const struct vfs_stat *st)
{
    if (st->type != VFS_FILE)
        return false;
    if (p && p->uid != 0) {
        uint16_t bits = st->uid == p->uid ? (st->mode >> 6) : st->gid == p->gid ? (st->mode >> 3) : st->mode;
        return (bits & 1) != 0;
    }
    return (st->mode & 0111) != 0; /* root still needs some execute bit */
}

/* Builds a new address space for `path` with the System V initial stack.
 * `cred` (may be NULL for the kernel) is checked for execute permission. */
static int load_image(const char *path, const struct argblock *a, const struct process *cred, uint32_t uid,
                      uint32_t gid, struct mm *out, struct loaded *res)
{
    struct vfs_stat st;
    int r = vfs_stat(path, &st);
    if (r)
        return r;
    if (st.type != VFS_FILE)
        return VFS_EISDIR;
    if (!exec_permitted(cred, &st))
        return AE_ACCES;
    if (st.size > PROC_MAX_IMAGE)
        return AE_NOEXEC;
    uint8_t *file = kmalloc(st.size ? st.size : 1);
    if (!file)
        return AE_NOMEM;
    size_t len = 0;
    r = vfs_read_file(path, file, st.size, &len);
    struct elf_image img;
    if (r == VFS_OK) {
        int e = elf_validate(file, len, AURORA_USER_BASE, AURORA_USER_LIMIT, &img);
        if (e != ELF_OK) {
            klog("proc: %s rejected: %s", path, elf_strerror(e));
            r = AE_NOEXEC;
        }
    }
    if (r) {
        kfree(file);
        return r;
    }
    if (mm_create(out)) {
        kfree(file);
        return AE_NOMEM;
    }
    bool ok = true;
    uint64_t top = 0;
    for (int i = 0; i < img.nseg && ok; i++) {
        const struct elf_segment *s = &img.seg[i];
        uint64_t start = ALIGN_DOWN(s->vaddr, PAGE_SIZE), end = ALIGN_UP(s->vaddr + s->memsz, PAGE_SIZE);
        uint8_t prot = VMA_R | (s->writable ? VMA_W : 0) | (s->executable ? VMA_X : 0);
        ok = mm_map(out, start, end - start, prot, VMA_IMAGE) == 0;
        /* File-backed bytes are copied now; the rest (.bss) is demand-zero. */
        if (ok && s->filesz)
            ok = copy_to_mm(out, s->vaddr, file + s->offset, s->filesz);
        top = MAX(top, end);
    }
    out->brk_base = out->brk = top + PAGE_SIZE; /* one unmapped page above the image */
    /* Stack region (demand-paged) with a guard page below it. */
    uint64_t stack_lo = AURORA_USER_STACK_TOP - AURORA_USER_STACK_SIZE;
    ok = ok && mm_map(out, stack_lo, AURORA_USER_STACK_SIZE, VMA_R | VMA_W, VMA_STACK) == 0;

    /* Initial stack, built in a kernel buffer then copied to the top:
     *   [strings][16 random bytes][program headers] ... aligned ...
     *   argc, argv[], NULL, envp[], NULL, auxv[], AT_NULL  <- RSP */
    size_t phbytes = (size_t)img.phnum * 56;
    int nauxv = 9;
    size_t ptrs = 1 + (size_t)a->argc + 1 + (size_t)a->envc + 1 + (size_t)nauxv * 2;
    size_t total = ALIGN_UP(a->bytes + 16 + 8 + phbytes, 16) + ALIGN_UP(ptrs * 8, 16); /* +8: phdr alignment */
    if (ok && total > AURORA_USER_STACK_SIZE / 2)
        ok = false;
    uint8_t *blk = ok ? kzalloc(total) : NULL;
    if (ok && !blk)
        ok = false;
    if (ok) {
        uint64_t base = AURORA_USER_STACK_TOP - 64 - total; /* 16-byte aligned */
        uint64_t *v = (uint64_t *)blk;
        size_t tail = ALIGN_UP(ptrs * 8, 16);
        uint64_t str_va = base + tail;
        memcpy(blk + tail, a->strings, a->bytes);
        uint64_t rnd_va = str_va + a->bytes;
        for (int i = 0; i < 2; i++) {
            uint64_t x = file_random64();
            memcpy(blk + tail + a->bytes + i * 8, &x, 8);
        }
        uint64_t ph_va = ALIGN_UP(rnd_va + 16, 8);
        memcpy(blk + (ph_va - base), file + img.phoff, phbytes);
        size_t k = 0;
        v[k++] = (uint64_t)a->argc;
        uint64_t sva = str_va;
        const char *sp = a->strings;
        for (int i = 0; i < a->argc + a->envc; i++) {
            if (i == a->argc)
                v[k++] = 0;
            v[k++] = sva;
            size_t n = strlen(sp) + 1;
            sva += n;
            sp += n;
        }
        if (a->envc == 0 && a->argc >= 0)
            v[k++] = 0; /* argv terminator when there is no environment */
        v[k++] = 0;      /* envp terminator */
        uint64_t aux[][2] = {
            { AURORA_AT_PHDR, ph_va }, { AURORA_AT_PHENT, 56 }, { AURORA_AT_PHNUM, img.phnum },
            { AURORA_AT_PAGESZ, PAGE_SIZE }, { AURORA_AT_ENTRY, img.entry }, { AURORA_AT_UID, uid },
            { AURORA_AT_GID, gid }, { AURORA_AT_RANDOM, rnd_va }, { AURORA_AT_NULL, 0 },
        };
        for (size_t i = 0; i < ARRAY_LEN(aux); i++) {
            v[k++] = aux[i][0];
            v[k++] = aux[i][1];
        }
        ok = k * 8 <= tail && copy_to_mm(out, base, blk, total);
        res->sp = base;
        res->entry = img.entry;
        kfree(blk);
    }
    kfree(file);
    if (!ok) {
        mm_destroy(out);
        return AE_NOMEM;
    }
    return 0;
}

/* ---- process slots and threads ---------------------------------------------- */

static struct process *slot_alloc(void)
{
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].state == PROC_FREE) {
            struct process *p = &procs[i];
            memset(p, 0, sizeof *p);
            for (;;) {
                int pid = next_pid++;
                if (next_pid > 99999)
                    next_pid = 2;
                if (!process_get(pid)) {
                    p->pid = pid;
                    break;
                }
            }
            return p;
        }
    }
    return NULL;
}

static void slot_release(struct process *p) { memset(p, 0, sizeof *p); }

static void set_tty(struct process *p, struct pty *t)
{
    if (p->tty == t)
        return;
    if (p->tty)
        pty_put(p->tty);
    p->tty = t;
    if (t)
        pty_ref(t);
}

void proc_set_ctty(struct process *p, struct pty *t) { set_tty(p, t); }

static void user_task_entry(void *arg)
{
    struct interrupt_frame fr = *(struct interrupt_frame *)arg;
    kfree(arg);
    /* The switch to this task loaded FS_BASE and the FPU state before it
     * got the BKL, while the creator (fork, thread_create) may still have
     * been filling them in on another CPU. Now that we hold the BKL the
     * creator is done: load them again. */
    struct task *self = sched_current();
    fpu_restore(self->fpu);
    wrmsr(MSR_FS_BASE, self->fs_base);
    bkl_release_all(); /* user mode never holds the BKL */
    cpu_cli();
    enter_user_frame(&fr);
}

static struct interrupt_frame *new_frame(uint64_t rip, uint64_t rsp)
{
    struct interrupt_frame *f = kzalloc(sizeof *f);
    if (f) {
        f->rip = rip;
        f->rsp = rsp;
        f->rflags = 0x202;
        f->cs = USER_CS;
        f->ss = USER_SS;
    }
    return f;
}

/* Adds a thread that enters user mode with frame `fr` (consumed). */
static struct task *add_thread(struct process *p, struct interrupt_frame *fr, int tid)
{
    if (p->nthreads >= PROC_MAX_THREADS) {
        kfree(fr);
        return NULL;
    }
    struct task *t = task_create(p->name, user_task_entry, fr, p);
    if (!t) {
        kfree(fr);
        return NULL;
    }
    t->tid = tid;
    p->threads[p->nthreads++] = t;
    return t;
}

static void name_from_path(struct process *p, const char *path)
{
    const char *base = strrchr(path, '/');
    str_copy(p->name, base ? base + 1 : path, sizeof p->name);
}

/* Common creation for kernel starts and spawn. */
static int create_process(const char *path, const struct argblock *a, struct process *parent,
                          const struct proc_start *ks, const struct aurora_spawn *sa)
{
    struct process *p = slot_alloc();
    if (!p)
        return AE_PROCLIM;
    uint32_t uid = parent ? parent->uid : ks->uid, gid = parent ? parent->gid : ks->gid;
    struct loaded ld;
    p->state = PROC_RUNNING; /* reserve the slot while loading */
    int r = load_image(path, a, parent, uid, gid, &p->mm, &ld);
    if (r) {
        slot_release(p);
        return r;
    }
    name_from_path(p, path);
    args_summary(a, p->args, sizeof p->args);
    p->uid = uid;
    p->gid = gid;
    p->umask = parent ? parent->umask : 022;
    p->start_ms = time_ms();
    if (parent) {
        p->parent = parent->pid;
        p->pgid = parent->pgid;
        p->sid = parent->sid;
        set_tty(p, parent->tty);
        str_copy(p->cwd, parent->cwd, sizeof p->cwd);
        for (int i = 0; i < AURORA_NSIG; i++)
            if (parent->sig[i].handler == AURORA_SIG_IGN)
                p->sig[i].handler = AURORA_SIG_IGN; /* caught signals reset to default */
        /* Descriptors: the fd map, or inherit everything not close-on-exec. */
        if (sa) {
            for (uint32_t i = 0; i < sa->nfds && i < AURORA_SPAWN_FDS; i++) {
                struct file *f = proc_fd_get(parent, sa->fd_map[i]);
                if (f) {
                    file_ref(f);
                    p->fds[i].f = f;
                }
            }
            if (sa->flags & AURORA_SPAWN_SETPGROUP)
                p->pgid = sa->pgroup ? sa->pgroup : p->pid;
            if (sa->flags & AURORA_SPAWN_SETSID) {
                p->sid = p->pgid = p->pid;
                struct file *f0 = p->fds[0].f;
                if (f0 && f0->kind == FK_TTY && f0->pty) {
                    set_tty(p, f0->pty);
                    f0->pty->sid = p->pid;
                    if (!(sa->flags & AURORA_SPAWN_BACKGROUND))
                        f0->pty->fg_pgrp = p->pid;
                } else {
                    set_tty(p, NULL);
                }
            }
            if (sa->flags & AURORA_SPAWN_SETSIGDEF)
                for (int i = 1; i < AURORA_NSIG; i++)
                    if (sa->sigdefault & (1ull << (i - 1)))
                        p->sig[i].handler = AURORA_SIG_DFL;
        } else {
            for (int i = 0; i < PROC_MAX_FDS; i++)
                if (parent->fds[i].f && !parent->fds[i].cloexec) {
                    file_ref(parent->fds[i].f);
                    p->fds[i].f = parent->fds[i].f;
                }
        }
    } else {
        p->parent = 0;
        p->pgid = p->sid = p->pid;
        str_copy(p->cwd, ks->cwd ? ks->cwd : "/home", sizeof p->cwd);
        if (ks->watch)
            p->watch = *ks->watch;
        if (ks->tty) {
            set_tty(p, ks->tty);
            ks->tty->sid = p->pid;
            ks->tty->fg_pgrp = p->pid;
            for (int i = 0; i < 3; i++) {
                struct file *f = file_new(FK_TTY, AURORA_O_RDWR);
                if (f) {
                    f->pty = ks->tty;
                    pty_ref(ks->tty);
                    p->fds[i].f = f;
                }
            }
        } else {
            int err;
            for (int i = 0; i < 3; i++)
                p->fds[i].f = file_open_device(p, "/dev/null", AURORA_O_RDWR, &err);
        }
    }
    struct interrupt_frame *fr = new_frame(ld.entry, ld.sp);
    struct task *t = fr ? add_thread(p, fr, p->pid) : NULL;
    if (!t) {
        close_all(p);
        set_tty(p, NULL);
        mm_destroy(&p->mm);
        slot_release(p);
        return AE_PROCLIM;
    }
    if (sa && (sa->flags & AURORA_SPAWN_SETSIGMASK))
        t->sigmask = sa->sigmask & ~((1ull << (AURORA_SIGKILL - 1)) | (1ull << (AURORA_SIGSTOP - 1)));
    else if (parent)
        t->sigmask = sched_current()->sigmask;
    klog("proc: started %s (pid %d, parent %d)", p->name, p->pid, p->parent);
    TEST_EVENT("PROC_START pid=%d name=%s", p->pid, p->name);
    return p->pid;
}

int process_start(const struct proc_start *s)
{
    char norm[VFS_PATH_MAX + 1];
    int r = vfs_normalize("/", s->path, norm, sizeof norm);
    if (r)
        return r;
    struct argblock a;
    r = argblock_from_kernel(&a, s->argv, s->envp);
    if (r)
        return r;
    r = create_process(norm, &a, NULL, s, NULL);
    argblock_free(&a);
    return r;
}

static int user_path(struct process *p, uint64_t upath, char *norm)
{
    char raw[VFS_PATH_MAX + 1];
    int r = copy_string_from_user(p, upath, raw, sizeof raw);
    if (r < 0)
        return r == AE_INVAL ? VFS_ENAMETOOLONG : r;
    return vfs_normalize(p->cwd, raw, norm, VFS_PATH_MAX + 1);
}

long proc_spawn(struct process *p, uint64_t upath, uint64_t uargv, uint64_t uenvp, uint64_t uattr)
{
    char norm[VFS_PATH_MAX + 1];
    int r = user_path(p, upath, norm);
    if (r)
        return r;
    struct aurora_spawn sa, *sap = NULL;
    if (uattr) {
        r = copy_from_user(p, &sa, uattr, sizeof sa);
        if (r)
            return r;
        sap = &sa;
    }
    struct argblock a;
    r = argblock_from_user(p, &a, uargv, uenvp);
    if (r)
        return r;
    r = create_process(norm, &a, p, NULL, sap);
    argblock_free(&a);
    return r;
}

long proc_fork(struct process *p, const struct interrupt_frame *f)
{
    struct process *c = slot_alloc();
    if (!c)
        return AE_PROCLIM;
    c->state = PROC_RUNNING;
    if (mm_clone(&c->mm, &p->mm)) {
        slot_release(c);
        return AE_NOMEM;
    }
    str_copy(c->name, p->name, sizeof c->name);
    str_copy(c->args, p->args, sizeof c->args);
    str_copy(c->cwd, p->cwd, sizeof c->cwd);
    c->parent = p->pid;
    c->pgid = p->pgid;
    c->sid = p->sid;
    c->uid = p->uid;
    c->gid = p->gid;
    c->umask = p->umask;
    memcpy(c->sig, p->sig, sizeof c->sig);
    set_tty(c, p->tty);
    c->start_ms = time_ms();
    for (int i = 0; i < PROC_MAX_FDS; i++) {
        if (p->fds[i].f) {
            file_ref(p->fds[i].f);
            c->fds[i] = p->fds[i];
        }
    }
    struct interrupt_frame *fr = kmalloc(sizeof *fr);
    struct task *self = sched_current();
    struct task *t = NULL;
    if (fr) {
        *fr = *f;
        fr->rax = 0; /* the child sees fork() return 0 */
        t = add_thread(c, fr, c->pid);
    }
    if (!t) {
        close_all(c);
        set_tty(c, NULL);
        mm_destroy(&c->mm);
        slot_release(c);
        return AE_PROCLIM;
    }
    fpu_save(t->fpu); /* the parent's live FPU/SSE registers */
    t->fs_base = self->fs_base;
    t->sigmask = self->sigmask;
    TEST_EVENT("PROC_START pid=%d name=%s", c->pid, c->name);
    return c->pid;
}

long proc_thread_create(struct process *p, uint64_t uattr)
{
    struct aurora_thread ta;
    int r = copy_from_user(p, &ta, uattr, sizeof ta);
    if (r)
        return r;
    if (ta.entry >= 0x0000800000000000ull || ta.stack >= 0x0000800000000000ull || (ta.stack & 7))
        return AE_INVAL;
    struct interrupt_frame *fr = new_frame(ta.entry, ta.stack);
    if (!fr)
        return AE_NOMEM;
    fr->rdi = ta.arg;
    int tid = next_pid++;
    struct task *t = add_thread(p, fr, tid);
    if (!t)
        return AE_PROCLIM;
    t->fs_base = ta.fsbase;
    t->clear_tid = ta.clear_tid;
    t->sigmask = sched_current()->sigmask;
    return tid;
}

/* Ends every other thread of p (execve) and waits until they are gone. */
static void kill_other_threads(struct process *p)
{
    struct task *self = sched_current();
    for (int i = 0; i < p->nthreads; i++) {
        struct task *t = p->threads[i];
        if (t && t != self) {
            t->exiting = true;
            sched_wake(t);
            sched_kick(t);
        }
    }
    while (p->nthreads > 1) {
        uint64_t f = irq_save();
        sched_wait_on(&p->nthreads, 20);
        irq_restore(f);
    }
}

long proc_execve(struct process *p, struct interrupt_frame *f, uint64_t upath, uint64_t uargv, uint64_t uenvp)
{
    char norm[VFS_PATH_MAX + 1];
    int r = user_path(p, upath, norm);
    if (r)
        return r;
    struct argblock a;
    r = argblock_from_user(p, &a, uargv, uenvp);
    if (r)
        return r;
    struct mm nm;
    struct loaded ld;
    r = load_image(norm, &a, p, p->uid, p->gid, &nm, &ld);
    if (r) {
        argblock_free(&a);
        return r;
    }
    if (p->nthreads > 1)
        kill_other_threads(p);
    /* Point of no return: switch to the new image. */
    struct mm old = p->mm;
    p->mm = nm;
    write_cr3(p->mm.pml4_phys);
    mm_destroy(&old);
    name_from_path(p, norm);
    args_summary(&a, p->args, sizeof p->args);
    argblock_free(&a);
    for (int i = 0; i < PROC_MAX_FDS; i++)
        if (p->fds[i].f && p->fds[i].cloexec)
            proc_fd_close(p, i);
    for (int i = 0; i < AURORA_NSIG; i++) {
        if (p->sig[i].handler != AURORA_SIG_IGN)
            p->sig[i].handler = AURORA_SIG_DFL;
        p->sig[i].flags = 0;
        p->sig[i].mask = 0;
    }
    struct task *t = sched_current();
    t->fs_base = 0;
    t->clear_tid = 0;
    wrmsr(MSR_FS_BASE, 0);
    fpu_init_state(t->fpu);
    fpu_restore(t->fpu);
    memset(f, 0, sizeof *f);
    f->rip = ld.entry;
    f->rsp = ld.sp;
    f->rflags = 0x202;
    f->cs = USER_CS;
    f->ss = USER_SS;
    TEST_EVENT("PROC_EXEC pid=%d name=%s", p->pid, p->name);
    return 0;
}

/* ---- exit ----------------------------------------------------------------- */

static void reap_zombie(struct process *c) { slot_release(c); }

static bool sigchld_ignored(const struct process *parent)
{
    return parent->sig[AURORA_SIGCHLD].handler == AURORA_SIG_IGN;
}

/* Last thread out: the process becomes a zombie (or is freed). */
static void teardown(struct process *p)
{
    struct task *t = sched_current();
    close_all(p);
    write_cr3(vmm_kernel_space());
    t->proc = NULL; /* from here on the task is a kernel task */
    mm_destroy(&p->mm);
    set_tty(p, NULL);
    p->status = p->exit_status;
    p->state = PROC_ZOMBIE;
    TEST_EVENT("PROC_EXIT pid=%d name=%s status=%d reason=%s", p->pid, p->name, p->status, p->reason);
    klog("proc: %s (pid %d) exited, status 0x%x%s%s", p->name, p->pid, p->status, p->reason[0] ? ": " : "",
         p->reason);
    /* Children go to init (or to the kernel, which reaps them). */
    struct process *init = init_pid && init_pid != p->pid ? process_get(init_pid) : NULL;
    if (init && init->state != PROC_RUNNING)
        init = NULL;
    bool init_has_zombie = false;
    for (int i = 0; i < MAX_PROCS; i++) {
        struct process *c = &procs[i];
        if (c->state == PROC_FREE || c->parent != p->pid || c == p)
            continue;
        if (init) {
            c->parent = init->pid;
            init_has_zombie |= c->state == PROC_ZOMBIE;
        } else if (c->state == PROC_ZOMBIE) {
            reap_zombie(c);
        } else {
            c->parent = 0;
        }
    }
    if (init_has_zombie) {
        signal_send(init, AURORA_SIGCHLD);
        sched_wake_all(init);
    }
    struct process *parent = process_get(p->parent);
    if (!parent || parent->state != PROC_RUNNING) {
        if (p->watch.exited)
            p->watch.exited(p->watch.ctx, p->pid, p->name, p->status, p->reason);
        reap_zombie(p); /* nobody will wait for it */
    } else if (sigchld_ignored(parent)) {
        reap_zombie(p);
        sched_wake_all(parent);
    } else {
        signal_send(parent, AURORA_SIGCHLD);
        sched_wake_all(parent);
    }
}

static void remove_thread(struct process *p, struct task *t)
{
    for (int i = 0; i < p->nthreads; i++) {
        if (p->threads[i] == t) {
            p->dead_ticks += t->cpu_ticks;
            p->threads[i] = p->threads[--p->nthreads];
            p->threads[p->nthreads] = NULL;
            break;
        }
    }
    sched_wake_all(&p->nthreads);
}

NORETURN void proc_thread_exit(void)
{
    struct task *t = sched_current();
    struct process *p = t->proc;
    if (!p)
        panic("proc_thread_exit outside a process");
    if (t->clear_tid && p->nthreads > 1) {
        uint32_t *w = mm_kptr(&p->mm, t->clear_tid, true);
        if (w) {
            *w = 0;
            sched_wake_n(w, 1);
        }
    }
    remove_thread(p, t);
    if (p->nthreads == 0)
        teardown(p);
    else
        t->proc = NULL;
    task_exit();
}

NORETURN void proc_exit(int status, const char *reason)
{
    struct process *p = process_current();
    if (!p)
        panic("proc_exit outside a process");
    if (!p->exiting) {
        p->exiting = true;
        p->exit_status = status;
        str_copy(p->reason, reason ? reason : "", sizeof p->reason);
        struct task *self = sched_current();
        for (int i = 0; i < p->nthreads; i++) {
            struct task *t = p->threads[i];
            if (t && t != self) {
                sched_wake(t);
                sched_kick(t);
            }
        }
    }
    proc_thread_exit();
}

/* ---- wait ----------------------------------------------------------------- */

long proc_waitpid(struct process *p, int pid, uint64_t ustatus, int options)
{
    for (;;) {
        bool any = false;
        for (int i = 0; i < MAX_PROCS; i++) {
            struct process *c = &procs[i];
            if (c->state == PROC_FREE || c->parent != p->pid || c == p)
                continue;
            if (pid > 0 && c->pid != pid)
                continue;
            if (pid == 0 && c->pgid != p->pgid)
                continue;
            if (pid < -1 && c->pgid != -pid)
                continue;
            any = true;
            if (c->state == PROC_ZOMBIE) {
                int32_t st = c->status;
                int cpid = c->pid;
                if (ustatus && copy_to_user(p, ustatus, &st, sizeof st))
                    return AE_FAULT;
                reap_zombie(c);
                return cpid;
            }
        }
        if (!any)
            return AE_CHILD;
        if (options & AURORA_WNOHANG)
            return 0;
        if (task_interrupted(sched_current()))
            return AE_INTR;
        uint64_t f = irq_save();
        sched_wait_on(p, 0);
        irq_restore(f);
    }
}

/* ---- futex ------------------------------------------------------------------- */

long proc_futex(struct process *p, uint64_t uaddr, int op, uint32_t val, int64_t timeout_ns)
{
    if (uaddr & 3)
        return AE_INVAL;
    /* Make the word private and writable first so its physical page (the
     * wait key) cannot change under a copy-on-write fault. */
    uint32_t *w = mm_kptr(&p->mm, uaddr, true);
    if (!w)
        return AE_FAULT;
    if (op == AURORA_FUTEX_WAKE)
        return sched_wake_n(w, val > 0x7fffffffu ? 0x7fffffff : (int)val);
    if (op != AURORA_FUTEX_WAIT)
        return AE_NOSYS;
    if (__atomic_load_n(w, __ATOMIC_SEQ_CST) != val)
        return AE_AGAIN;
    if (task_interrupted(sched_current()))
        return AE_INTR;
    uint64_t ms = timeout_ns < 0 ? 0 : (uint64_t)(timeout_ns + 999999) / 1000000;
    if (timeout_ns == 0)
        return AE_TIMEDOUT;
    uint64_t f = irq_save();
    bool woke = sched_wait_on(w, ms);
    irq_restore(f);
    if (!woke)
        return AE_TIMEDOUT;
    if (task_interrupted(sched_current()))
        return AE_INTR;
    return 0;
}

/* ---- boot ----------------------------------------------------------------- */

static void init_exited(void *ctx, int pid, const char *name, int status, const char *reason)
{
    UNUSED(ctx);
    UNUSED(name);
    klog("proc: init (pid %d) exited (status 0x%x %s); orphans are now reaped by the kernel", pid, status, reason);
    init_pid = 0;
}

static int64_t epoch_now(void) { return (int64_t)(boot_epoch + time_ms() / 1000); }

void process_init_clock(void)
{
    extern int64_t rtc_read_epoch(void);
    boot_epoch = (uint64_t)rtc_read_epoch();
    vfs_set_clock(epoch_now);
}

void process_start_init(void)
{
    static const struct proc_watch w = { init_exited, NULL };
    const char *argv[] = { "init", NULL };
    const char *envp[] = { "PATH=/system/bin", "HOME=/home", NULL };
    struct proc_start s = { .path = "/system/bin/init", .argv = argv, .envp = envp, .watch = &w, .cwd = "/" };
    int pid = process_start(&s);
    if (pid < 0) {
        klog("proc: init not started (%s); orphans are reaped by the kernel", process_spawn_error(pid));
        return;
    }
    init_pid = pid;
}
