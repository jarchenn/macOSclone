#ifndef AURORA_PROCESS_H
#define AURORA_PROCESS_H
#include "types.h"
#include "abi/aurora_abi.h"
#include "memory/uvm.h"
#include "fs/vfs.h"

/* User processes (0.6): one address space (struct mm), up to
 * PROC_MAX_THREADS threads (tasks), a descriptor table of shared open file
 * descriptions, credentials, a working directory, signal dispositions and a
 * place in the process tree (parent, process group, session, controlling
 * terminal).
 *
 * Process tree: processes started by the kernel (the Terminal app, init)
 * have parent 0 and report their exit through `watch`. When a process
 * exits, its children are re-parented to init (pid 1); if init is not
 * running they are re-parented to the kernel and reaped on exit.
 *
 * All of this state is protected by the BKL. */

#define MAX_PROCS 64
#define PROC_MAX_FDS 64
#define PROC_MAX_THREADS 16
#define PROC_MAX_IMAGE (8u * 1024u * 1024u)

enum proc_state { PROC_FREE = 0, PROC_RUNNING, PROC_ZOMBIE };

/* Exit notification for processes whose parent is the kernel. */
struct proc_watch {
    void (*exited)(void *ctx, int pid, const char *name, int status, const char *reason);
    void *ctx;
};

struct file;
struct pty;
struct task;
struct interrupt_frame;

struct fdesc {
    struct file *f;
    bool cloexec;
};

struct sigdisp {
    uint64_t handler, restorer, mask;
    uint32_t flags;
};

struct process {
    int pid;
    int state;
    char name[32];
    char args[128];      /* argv joined with spaces, for ps */
    int parent;          /* 0 = the kernel */
    int pgid, sid;
    uint32_t uid, gid, umask;
    struct mm mm;
    struct task *threads[PROC_MAX_THREADS];
    int nthreads;
    struct fdesc fds[PROC_MAX_FDS];
    char cwd[VFS_PATH_MAX + 1];
    struct sigdisp sig[AURORA_NSIG];
    uint64_t sigpending;  /* process-directed signals */
    struct pty *tty;      /* controlling terminal */
    struct proc_watch watch;
    bool exiting;         /* exit in progress: every thread leaves */
    int exit_status;      /* wait status once exiting */
    int status;           /* wait status of a zombie */
    char reason[96];      /* human-readable cause of death ("" for exit()) */
    char fault[96];       /* description of the last synchronous fault */
    int fault_sig;        /* signal that fault raised */
    uint64_t start_ms;
    uint64_t syscalls;
    uint64_t dead_ticks;  /* CPU ticks of threads that already exited */
    uint64_t alarm_ms;    /* SIGALRM deadline, 0 = none */
    uint64_t user_pages_peak;
};

/* Creation. argv/envp are NULL-terminated kernel arrays. A process with a
 * pty becomes a session leader with that controlling terminal and fds 0-2
 * on it. Returns the pid or a negative AE_ or VFS error. */
struct proc_start {
    const char *path;
    const char *const *argv;
    const char *const *envp;
    struct pty *tty;
    const struct proc_watch *watch;
    uint32_t uid, gid;
    const char *cwd;
};
int process_start(const struct proc_start *s);
/* Makes t the controlling terminal of p (TIOCSCTTY). */
void proc_set_ctty(struct process *p, struct pty *t);

struct process *process_current(void);
struct process *process_get(int pid);
struct process *process_at(int index); /* iteration over live slots */
phys_addr_t process_space(const struct process *p);
int process_count(void);
int process_thread_count(void);
const char *process_spawn_error(int err);
uint64_t process_cpu_ticks(const struct process *p);

/* True when the current task's blocking operation should end early: a
 * signal it will act on is pending, or its process is exiting. */
bool process_kill_pending(const struct process *p);
bool task_interrupted(struct task *t);

/* Signals */
int signal_send(struct process *p, int sig);          /* process-directed */
int signal_send_thread(struct task *t, int sig);
int signal_send_group(int pgid, int sig);
int process_kill(int pid);                            /* SIGKILL (Terminal) */
/* Runs on every return to user mode: thread exit, signal delivery. */
void signal_return_to_user(struct interrupt_frame *f);
long signal_sigreturn(struct interrupt_frame *f);
/* A CPU exception in user mode: resolves page faults, otherwise queues the
 * matching signal (SIGSEGV, SIGILL, SIGFPE, ...). */
void process_user_exception(struct interrupt_frame *f);

/* Called by the syscall layer. */
long proc_fork(struct process *p, const struct interrupt_frame *f);
long proc_execve(struct process *p, struct interrupt_frame *f, uint64_t upath, uint64_t uargv, uint64_t uenvp);
long proc_spawn(struct process *p, uint64_t upath, uint64_t uargv, uint64_t uenvp, uint64_t uattr);
long proc_waitpid(struct process *p, int pid, uint64_t ustatus, int options);
long proc_thread_create(struct process *p, uint64_t uattr);
NORETURN void proc_exit(int status, const char *reason);   /* whole process */
NORETURN void proc_thread_exit(void);                       /* calling thread */
long proc_futex(struct process *p, uint64_t uaddr, int op, uint32_t val, int64_t timeout_ns);
int proc_fd_install(struct process *p, struct file *f, int min, bool cloexec);
struct file *proc_fd_get(struct process *p, int fd);
int proc_fd_close(struct process *p, int fd);

/* User memory access through the current process's address space. */
bool user_range_ok(struct process *p, uint64_t addr, uint64_t len, bool write);
int copy_from_user(struct process *p, void *dst, uint64_t src, uint64_t len);
int copy_to_user(struct process *p, uint64_t dst, const void *src, uint64_t len);
int copy_string_from_user(struct process *p, uint64_t src, char *dst, size_t cap);

void syscall_dispatch(struct interrupt_frame *f);
/* Maps internal VFS/AE errors to -errno for user space. */
long errno_from(long kernel_err);

/* Boot: starts /system/bin/init (pid 1), which reaps orphans. */
void process_start_init(void);
void process_init_clock(void);   /* reads the RTC; file times use it */
uint64_t process_boot_epoch(void); /* seconds since 1970 at boot (RTC) */

#endif
