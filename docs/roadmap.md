# Roadmap

Milestone status, with evidence in `STATUS.md`. "Done" means implemented and
tested in the guest. Partial items list exactly what is missing.

From 0.5 on, the plan is the "OS Roadmap: 0.5 and Beyond" document
(2026-11-06): 0.5 SMP and ACPI, 0.6 POSIX-ish userland, 0.7 desktop
maturity, 0.8 real hardware, 0.9 hardening, 1.0 stable release, then the 1.x
series and 2.0 (isolated drivers and capabilities). Releases go in order; a
release starts only once the previous one's acceptance matrix passes. Design
decisions are recorded in [decisions.md](decisions.md).

## 0.1: Bootable desktop (done)

Limine boot, kernel core, software desktop, four built-in apps, RAM `/home`.

## 0.2: User-process isolation (done, with one item deferred)

* Done: tasks with per-task kernel stacks and TSS.RSP0, preemptive
  round-robin scheduling of user mode on the PIT, ring-3 segments, an
  `int 0x80` syscall table with page-walk pointer validation, per-process page
  tables with a shared higher half, ELF64 validation and loading, a user
  libc, and 15 user programs. Faults kill only the faulting process. Tests
  show that one process cannot read another's memory.
* Deferred: **moving the desktop and apps out of the kernel** into a
  user-space service. This needs IPC and shared surfaces (now planned for
  0.7a). The LAPIC timer and IOAPIC arrived in 0.5.

## 0.3: Persistent storage (done)

A VirtIO block driver with timeouts, and PFS, a shadow-paged copy-on-write
filesystem on a separate data disk, with a commit after every change.
Power-loss tests run in the host tests (a crash after every write) and in
the guest (hard QEMU kills). A damaged disk is never reformatted.

## 0.4: Networking (done, client side)

virtio-net, ARP, IPv4, ICMP, UDP, DHCP, DNS, a TCP client and an HTTP/1.1
`fetch`. Not done: TCP listen/accept, congestion control, fragment
reassembly, IPv6, TLS.

## 0.5: SMP and ACPI (done, with recorded deviations)

See [smp.md](smp.md). Done:
* 0.5a: ACPI table parser (checksums, MADT, FADT, HPET; BIOS-area RSDP
  fallback), with all hard-coded APIC addresses replaced by MADT values.
* 0.5b: per-CPU data through GS with SWAPGS; per-CPU GDT/TSS/IST.
* 0.5c: INIT-SIPI-SIPI AP start-up with timeouts. A CPU that fails is
  skipped.
* 0.5d: ticket spinlocks, IRQ-save variants, a lock-order validator with
  cycle detection, a hold-time watchdog, and a sleeping-with-spinlock check.
* 0.5e: allocator, heap, VMM and scheduler locks; the BKL for everything
  older.
* 0.5f: TLB shootdown IPIs.
* 0.5g: SMP scheduling with reschedule IPIs.
* 0.5h: power button via SCI, FADT reset with fallbacks, panic halts other
  CPUs.

Deviations (see decisions.md): no AML interpreter (D-0.5-1); the BKL still
covers the VFS, network, drivers, desktop and the user page-fault path
(D-0.5-2); one global run queue instead of per-CPU queues (D-0.5-3).
Deferred by the roadmap: NUMA, CPU hotplug, S3, frequency scaling, x2APIC,
kernel preemption.

## 0.6: POSIX-ish userland (done, with recorded deviations)

Details in [userland.md](userland.md); decisions D-0.6-1..6 in
[decisions.md](decisions.md); evidence in `STATUS.md` (`make posix`).

* 0.6a ABI and toolchain: clang for `x86_64-unknown-none-elf`, a sysroot,
  System V initial stack with argc/argv/envp/auxv, TLS through FS base,
  per-thread FPU/SSE state. Done.
* 0.6b C library: newlib 4.6.0 with Aurora's OS layer; `mmap`/`munmap`/
  `mprotect`, `clock_gettime`, process calls, libm. Done.
* 0.6c Process model: native `posix_spawn`, `fork` with copy-on-write,
  `execve`, `waitpid` with exit status, orphans re-parented to init;
  threads + futex, pthreads. Done.
* 0.6d File descriptors: one table for files, directories, pipes, sockets
  and devices; `dup`/`dup2`, close-on-exec, `O_NONBLOCK`, `fcntl` subset,
  `poll` (and `select` in libc). Done.
* 0.6e Pipes and signals: as specified, including the signal frame,
  `sigreturn`, masks and EINTR. Done.
* 0.6f VFS completion: `unlink`, atomic `rename`, `mkdir`/`rmdir`,
  `truncate`, directory iteration, per-process cwd. Done (no symlinks;
  optional in the roadmap).
* 0.6g Permissions: uid/gid on processes, mode bits on files (persisted),
  enforced in open and exec. Done; the session runs as root (D-0.6-5).
* 0.6h Terminal and shell: pty with a line discipline, VT100-subset
  Terminal, a shell with pipelines and redirection, small coreutils. Done
  (original shell and utilities, D-0.6-4).
* Acceptance tests: all nine pass at `-smp 1` and `-smp 4` (`make posix`);
  the Lua test suite is run in its portable subset (`_U`).

Deferred by the roadmap: dynamic linking, shared memory, job control,
multiple terminals, Linux ABI compatibility. Still open from 0.5: user page
faults are resolved under the BKL (D-0.6-6).

## 0.7 and later

Not started. 0.7 (desktop maturity) needs shared-memory surfaces first; 0.8
needs a physical reference machine. These are tracked against the roadmap
document's acceptance tests.

## Later items (from the 0.1 roadmap)

| Item | Status |
|---|---|
| Window resizing | **done**: drag the lower-right grip; an outline shows the new size; minimum 320×180 (`window-resize`) |
| Delete/rename in the VFS | **done**: `vfs_remove` and `vfs_rename`; Terminal `rm` and `mv`; persisted on disk |
| Clipboard | **done** (text): Notes selection with Shift+arrows, mouse drag and double-click; Ctrl+A/C/X/V; Terminal Ctrl+V pastes the first line. Guest-tested, including double-click and drag selection (`clipboard-copy-paste`). The clipboard is a kernel buffer, not a service |
| Keyboard LEDs | **done**: Caps Lock LED (`caps-lock-led`); Num Lock and Scroll Lock are not tracked |
| ACPI power-off | **done**: RSDP → RSDT/XSDT → FADT → `\_S5` found by a byte scan of the DSDT (no AML interpreter); Shut Down menu item and `poweroff`; syncs `/home` first (`acpi-poweroff`) |
| Keyboard layouts | not started (US only); planned for 0.7g |
| File dialogs | not started (Notes uses Save As with a name field; Open goes through Files); planned for 0.7f |
| Richer text (proportional, anti-aliased fonts) | not started (Spleen bitmap fonts); planned for 0.7b |
| More graphics modes | not started (uses the mode Limine sets); planned for 0.7h |
| Audio | not started; planned for 0.8h |
| USB HID | not started (PS/2 only); planned for 0.8e |
| SMP | **done in 0.5** (1–16 CPUs; 1/2/4/8 tested) |
| FPU/SSE in user programs | not started (no FPU state is saved) |

Out of scope as independent research-scale projects: browser engine, package
ecosystem, Apple Silicon port, macOS binary compatibility.
