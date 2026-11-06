#ifndef AURORA_PROCESS_H
#define AURORA_PROCESS_H
#include "types.h"
#include "abi/aurora_abi.h"

/* User processes: a private lower-half address space, one task running in
 * ring 3, a small descriptor table and a console (the Terminal that started
 * it, or the parent's console). */

#define MAX_PROCS 32
#define PROC_MAX_FDS 16
#define PROC_IN_CAP 1024
#define PROC_MAX_IMAGE (4u * 1024u * 1024u)

enum proc_state { PROC_FREE = 0, PROC_RUNNING, PROC_ZOMBIE };
enum fd_kind { FD_NONE = 0, FD_CONSOLE, FD_FILE, FD_SOCKET };

struct proc_console {
    void (*output)(void *ctx, int pid, const char *data, size_t len);
    void (*exited)(void *ctx, int pid, const char *name, int status, const char *reason);
    void *ctx;
};

struct proc_fd {
    uint8_t kind;
    int handle;
};

struct task;
struct interrupt_frame;

struct process {
    int pid;
    int state;
    char name[32];
    char args[AURORA_USER_ARGS_MAX];
    int parent; /* parent pid, 0 if started by the kernel (Terminal) */
    uint64_t entry;
    uint64_t *pml4;
    phys_addr_t pml4_phys;
    struct task *task;
    struct proc_console con;
    char in[PROC_IN_CAP];
    uint32_t in_r, in_w;
    bool in_eof;
    struct proc_fd fds[PROC_MAX_FDS];
    bool kill_pending;
    int status;
    char reason[96];
    uint64_t start_ms;
    uint64_t user_pages;
    uint64_t syscalls;
};

/* Returns the new pid, or a negative error (VFS error or AE_*). */
int process_spawn(const char *path, const char *args, const struct proc_console *con, int parent);
struct process *process_current(void);
struct process *process_get(int pid);
struct process *process_at(int index); /* iteration over live slots */
phys_addr_t process_space(const struct process *p);
bool process_kill_pending(const struct process *p);
int process_kill(int pid);
int process_input(int pid, const char *data, size_t len, bool eof);
int process_wait(struct process *self, int child);
NORETURN void process_exit(int status, const char *reason);
NORETURN void process_fault(const struct interrupt_frame *f);
/* Exits the current process if a kill is pending (called before returning
 * to user mode). */
void process_check_kill(void);
const char *process_spawn_error(int err);
int process_count(void);

/* User memory access. All check presence, the user bit and (for writes) the
 * writable bit of every page in the current address space. */
bool user_range_ok(struct process *p, uint64_t addr, uint64_t len, bool write);
int copy_from_user(struct process *p, void *dst, uint64_t src, uint64_t len);
int copy_to_user(struct process *p, uint64_t dst, const void *src, uint64_t len);
int copy_string_from_user(struct process *p, uint64_t src, char *dst, size_t cap);

void syscall_dispatch(struct interrupt_frame *f);

#endif
