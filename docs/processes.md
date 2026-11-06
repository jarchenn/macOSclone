# Processes and scheduling (0.2)

Aurora runs ELF64 programs in ring 3, each in its own address space. The
desktop and its four apps are still compiled into the kernel and run as a
kernel task. Only the programs in `/system/bin` are user processes.

## Tasks and the scheduler (`core/sched.c`)

* Up to 64 tasks. Task 0 is the desktop, running on the boot stack. There is
  one idle task, and each user process has one task.
* Each task has a 32 KiB kernel stack with an unmapped guard page. Stacks
  live in fixed slots at `VMM_STACK_BASE + 1 MiB`.
* `context_switch` (`arch/x86_64/switch.S`) saves rbp, rbx, r12–r15 and
  rflags. A new task starts in `task_trampoline`.
* Round-robin with a 3-tick slice (12 ms at 250 Hz). The PIT IRQ calls
  `sched_tick`.
* **The kernel is not preemptible.** The timer only switches tasks when it
  interrupts user mode or the idle task. Kernel code gives up the CPU
  explicitly: `sched_block`, `sched_sleep_ms`, `sched_wait_on`, or a
  blocking syscall. This keeps the single-CPU kernel free of locks.
* The desktop blocks in `sched_block` when it has nothing to do. PS/2 IRQs
  and the timer wake it.

## Address spaces (`memory/virtual.c`)

* Every process has its own PML4. Entries 256–511 (the kernel half) are
  created once at boot and shared, so a kernel mapping added later is
  visible in every process.
* The user half is mapped with the U bit. Program segments must lie in
  `[0x400000, 0x700000000000)`, and the 128 KiB stack sits below
  `0x7fffffff0000`. Arguments (at most 256 bytes) are copied to the top of
  the stack.
* `vmm_space_destroy` frees every user page and page table.
  `process-memory-reclaimed` checks that free physical memory returns to the
  same value after 10 process lifetimes.

## Entering and leaving user mode

* GDT: 0x08 kernel code, 0x10 kernel data, 0x18 user data (DPL 3), 0x20
  user code (DPL 3), 0x28 TSS. `TSS.RSP0` is set to the running task's
  kernel stack on every switch.
* `enter_user` performs an `iretq` to CS 0x23 / SS 0x1B with IF set.
* System calls use `int 0x80` through a DPL-3 interrupt gate. The handler
  re-enables interrupts. The ABI is in `kernel/include/abi/aurora_abi.h`:
  31 call numbers covering exit, read/write, open/close, spawn/wait,
  sleep/yield, directory and stat calls, unlink, sync, sockets, DNS, ping and
  netinfo.
* User pointers are checked by walking the process's page tables, page by
  page. A pointer must be user-accessible (and writable for output buffers)
  and must stay below the user limit. Data passes through kernel bounce
  buffers. A bad pointer returns `AE_FAULT` (-100) and never faults in the
  kernel. Test: `badptr`, 10 checks.

## Faults and killing

A CPU exception taken in user mode kills only the faulting process. The kernel
records a reason (for example "segmentation fault: write unmapped at 0"), sets
the exit status to 128 + vector and frees the address space.
`user-programs-and-faults` covers five kinds of fault: NULL write, kernel-address
read, stack overflow, invalid opcode and #GP. `kill` and Ctrl+C set a kill
flag. The process exits when it next returns to user mode or wakes from a
blocking call; blocked socket and stdin reads return `AE_INTR`.

## ELF loading (`core/elf.c`)

`elf_validate` is portable and covered by host tests (`elf` suite). It checks
ELF64 little-endian x86-64 `ET_EXEC`, header sizes, a program-header table
inside the file, `PT_LOAD` segments inside the file and the user range, no
overlap, filesz ≤ memsz, no W+X segments, an entry point inside an executable
segment, and an image of at most 4 MiB.

## User runtime (`user/`)

Programs link at 0x400000 with `user/linker.ld` and `crt0.S`. `libc.c`
provides syscall wrappers, a 128 KiB bump allocator, `printf` (the kernel's
own formatter), string functions, `readline` and IPv4 helpers. User
programs are built with `-mgeneral-regs-only`. **The kernel does not save FPU
or SSE state, so user programs cannot use floating point.**

Programs in `/system/bin`: `hello`, `counter`, `spin`, `crash`, `memtest`,
`badptr`, `cat`, `ls`, `upper`, `sh` (a tiny user-space shell), `ping`,
`nslookup`, `fetch` (an HTTP/1.1 GET client), `tcpecho` and `udpecho`.

## Terminal integration

Type a program name to run it in the foreground. The Terminal sends typed
lines to its stdin (1 KiB ring). Ctrl+D sends end-of-file and Ctrl+C kills it.
A trailing `&` runs it in the background. `ps` lists processes and `kill PID`
ends one. Output goes into the Terminal window.

## Limits and what is not done

* At most 32 processes and 16 file descriptors each. There is no `fork`,
  `exec`, `mmap`, signals, pipes or IPC. `spawn` + `wait` is the only process
  API.
* The apps (Terminal, Files, Notes, Settings) and the compositor are still
  kernel code. The roadmap's "desktop as a user-space service" is **not
  done**, because it needs IPC and shared surfaces.
* There is one CPU (no SMP). The timer is the PIT; the LAPIC timer and
  IOAPIC are not used.
* `syscall`/`sysret` is not used. `int 0x80` is simpler to make safe and is
  fast enough here.
