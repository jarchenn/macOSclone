# Processes and scheduling (0.2, extended in 0.6)

Aurora runs ELF64 programs in ring 3, each in its own address space. The
desktop and its four apps are still compiled into the kernel and run as a
kernel task. Only the programs in `/system/bin` are user processes. Since
0.6 processes have threads, fork/exec, signals, descriptors and a C library:
see [userland.md](userland.md) for the POSIX layer; this page covers the
kernel mechanics.

## Tasks and the scheduler (`core/sched.c`)

* Up to 160 tasks. Task 0 is the desktop, running on the boot stack. Each CPU
  has an idle task, and each user thread is a task (up to 16 per process,
  64 processes).
* Each task has a 32 KiB kernel stack with an unmapped guard page. Stacks
  live in fixed slots at `VMM_STACK_BASE + 1 MiB`.
* `context_switch` (`arch/x86_64/switch.S`) saves rbp, rbx, r12–r15 and
  rflags. A new task starts in `task_trampoline`. For user threads the
  switch also saves and restores the FXSAVE area (x87/SSE state) and loads
  FS_BASE (thread-local storage).
* Round-robin with a 3-tick slice (12 ms at 250 Hz) on every CPU. Each
  CPU's LAPIC timer calls `sched_tick` (the PIT before the APIC is up). See
  [smp.md](smp.md) for SMP scheduling, locking and the big kernel lock.
* **The kernel is not preemptible.** The timer only switches tasks when it
  interrupts user mode or the idle task. Kernel code gives up the CPU
  explicitly: `sched_block`, `sched_sleep_ms`, `sched_wait_on`, or a
  blocking syscall.
* The desktop blocks in `sched_block` when it has nothing to do. PS/2 IRQs
  and the timer wake it.

## Address spaces (`memory/virtual.c`, `memory/uvm.c`)

* Every process has its own PML4. Entries 256–511 (the kernel half) are
  created once at boot and shared, so a kernel mapping added later is
  visible in every process.
* The user half is described by regions (image segments, heap, stack,
  mmap). Program segments must lie in `[0x400000, 0x700000000000)`; the
  stack region is 8 MiB below `0x7fffffff0000`; mmap searches from
  `0x100000000000`. Pages are allocated on first touch.
* fork shares pages copy-on-write using share counts in the page
  allocator; `vmm_space_destroy` releases every user page by count and frees
  every page table. `forktest` checks that 1000 fork/exit cycles leave the
  free page count unchanged.

## Entering and leaving user mode

* GDT: 0x08 kernel code, 0x10 kernel data, 0x18 user data (DPL 3), 0x20
  user code (DPL 3), 0x28 TSS. `TSS.RSP0` is set to the running task's
  kernel stack on every switch.
* `enter_user` performs an `iretq` to CS 0x23 / SS 0x1B with IF set.
* System calls use `int 0x80` through a DPL-3 interrupt gate. The handler
  re-enables interrupts. The ABI is in `kernel/include/abi/aurora_abi.h`
  (0.6: about 80 calls with POSIX semantics, results `-errno`).
* User pointers are checked against the process's regions page by page and
  faulted in if needed (`mm_prepare`); output buffers must be writable,
  which also breaks copy-on-write before the kernel writes. A bad pointer
  returns EFAULT and never faults in the kernel. Test: `badptr`, 10 checks.
* Every return to user mode passes `signal_return_to_user`, which ends
  exiting threads and delivers pending signals.

## Faults, signals and killing

A user page fault inside a region is resolved (demand page, copy-on-write)
and the instruction retried. Any other CPU exception in user mode becomes a
signal for the thread (SIGSEGV, SIGILL, SIGFPE, SIGTRAP, SIGBUS); with no
handler the process dies and the kernel keeps a reason (for example
"segmentation fault: write unmapped at 0") next to the wait status, which
is the signal number. `user-programs-and-faults` covers five kinds of
fault. The Terminal's `kill` sends SIGKILL; Ctrl+C is SIGINT through the
pty. A signal wakes a blocked thread (its call returns EINTR) or, for a
thread running on another CPU, interrupts that CPU with an IPI.

## ELF loading (`core/elf.c`)

`elf_validate` is portable and covered by host tests (`elf` suite). It checks
ELF64 little-endian x86-64 `ET_EXEC`, header sizes, a program-header table
inside the file, `PT_LOAD` segments inside the file and the user range, no
overlap, filesz ≤ memsz, no W+X segments, an entry point inside an executable
segment, and an image of at most 8 MiB (the 0.6 loader limit; the validator
allows 16 MiB). PT_TLS headers are allowed; the program headers are copied
onto the initial stack for AT_PHDR.

## User runtime and Terminal

The user runtime is newlib plus Aurora's libc layer, and programs run on the
Terminal's pseudo-terminal: see [userland.md](userland.md). Type a program
name in the Terminal to run it in the foreground; lines with shell syntax
go to `sh -c`; a trailing `&` runs a program in the background; `ps` lists
processes (pid, parent, uid, threads, CPU time, resident memory) and `kill
PID` sends SIGKILL.

## Limits and what is not done

* At most 64 processes, 16 threads and 64 descriptors each. No shared
  memory, job control or IPC beyond pipes, signals and sockets (0.6).
* The apps (Terminal, Files, Notes, Settings) and the compositor are still
  kernel code. The roadmap's "desktop as a user-space service" is **not
  done**, because it needs IPC and shared surfaces.
* System calls and user page faults run under the big kernel lock (0.5), so
  they are serialized across CPUs; user-mode code runs in parallel.
* `syscall`/`sysret` is not used. `int 0x80` is simpler to make safe and is
  fast enough here.
