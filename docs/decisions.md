# Decision records

Short records for the "design decisions to settle first" in the roadmap.
Each gives the choice, the alternatives and the reason. A later release may
revisit a decision; when it does, it adds a new record instead of editing
the old one.

## D-0.5-1: no AML interpreter yet

* **Choice:** keep finding `\_S5` with a byte scan of the DSDT. Handle the
  power button through the fixed-feature PM1 event (no AML needed).
* **Alternatives:** integrate uACPI or LAI (the roadmap's suggestion);
  write an interpreter.
* **Why:** power-off and the power button are the only things 0.5 needs
  from ACPI, and both work on the reference machine (QEMU) without AML.
  Adding uACPI means vendoring about 30k lines of third-party code plus a
  kernel API layer (mutexes, events, memory mapping, PCI access) that the
  kernel does not have yet in a stable form. Revisit in 0.8: real firmware
  often has `\_S5` behind methods or conditionals that a byte scan cannot
  follow, and the power button may be a GPE device there.

## D-0.5-2: big kernel lock as a bridge

* **Choice:** a recursive, per-task BKL covers system calls, the VFS, PFS,
  the drivers, the network stack, the desktop and process management. The
  scheduler, timer, IRQ handlers, IPIs, page allocator, heap and kernel
  page-table updates have their own locks and never take the BKL.
* **Alternatives:** audit and lock every subsystem before starting APs.
* **Why:** it makes APs useful at once (user code runs in parallel) without
  a big-bang rewrite. The roadmap allows a BKL during 0.5c as long as the
  scheduler, allocator and fault paths stop taking it. The first two are
  done. **The user page-fault path still takes it**, because a fault means
  process teardown; it will lose the BKL when demand paging arrives (0.6
  `mmap`/COW).

## D-0.5-3: one global run queue

* **Choice:** a single task table scanned round-robin under `sched_lock`,
  with `on_cpu` ownership and reschedule IPIs.
* **Alternatives:** per-CPU run queues with periodic balancing that locks
  queues in CPU-ID order (the roadmap's design).
* **Why:** with at most 64 tasks and 8 CPUs, one lock is not the bottleneck
  (the BKL is). A global queue balances perfectly and needs no migration
  logic. Per-CPU queues become worthwhile with many tasks or CPU affinity
  for caches; revisit after 1.0's performance baseline.

## D-0.5-4: kernel stays non-preemptible

* **Choice:** the timer preempts only user mode and the idle task (roadmap
  decision).
* **Why:** it removes a class of SMP bugs (preemption with locks held,
  per-CPU data races) while the lock audit is incomplete.

## D-0.5-5: all device IRQs to CPU 0

* **Choice:** route every IOAPIC pin to the boot CPU (roadmap decision).
* **Why:** the devices are polled (virtio) or low-rate (PS/2, SCI), so
  distributing them gains nothing yet and adds failure modes. Revisit when
  an IRQ-driven NIC or disk lands.

## D-0.5-6: xAPIC only

* **Choice:** MMIO xAPIC. If firmware hands over in x2APIC mode, stay on the
  PIC with one CPU.
* **Why:** Limine on QEMU leaves the xAPIC enabled; x2APIC is deferred
  (roadmap). The fallback keeps the system usable instead of failing.

## D-0.6-1: newlib as the C library, built from its sources directly

* **Choice:** newlib 4.6.0 (pinned, SHA-512 checked), with Aurora's own OS
  layer (`user/libc/src`: system-call wrappers, start-up, TLS, pthreads,
  locks, posix_spawn, termios). `tools/userland.py` compiles the file lists
  in newlib's `Makefile.inc` fragments for Aurora's configuration; newlib's
  configure script and makefiles are never run. A handful of installed
  headers get small, listed patches (feature macros, `SA_RESTART`,
  `getline`, `lstat`, sigset macros), all in `userland.py`.
* **Alternatives:** mlibc (cleaner OS layer, but C++ and Meson); extending
  Aurora's tiny libc (months of work for `printf("%g")`, `strtod`, locale,
  regex...). The user picked newlib.
* **Why:** newlib covers ISO C plus a useful slice of POSIX (stdio, regex,
  glob, getopt, time) and libm, and its OS boundary is a small set of
  functions. Running its autotools build on Windows would be slow and
  fragile; the Makefile.inc lists are simple enough to evaluate directly.
  Two newlib configuration choices matter: `_REENT_THREAD_LOCAL` (errno and
  friends are compiler TLS, so threads need FS-based TLS from day one) and
  no positional printf arguments (newlib's implementation takes the address
  of a `va_list` parameter, which is wrong on x86-64).

## D-0.6-2: Aurora's own system-call numbers

* **Choice:** `int $0x80` with Aurora numbering and POSIX semantics; errno
  values, open flags, signal numbers, `struct termios` and the wait status
  encoding equal newlib's so the libc passes them through. A compile-time
  check (`user/libc/src/abi_check.c`) fails the build on any mismatch.
* **Alternatives:** the Linux x86-64 ABI.
* **Why:** the roadmap advises against promising Linux binary
  compatibility. Matching newlib's constants instead of Linux's removes a
  translation layer. (The check caught a real mismatch during bring-up:
  newlib numbers SIGUSR1/SIGUSR2/SIGWINCH 30/31/28, not 25/26/24.)

## D-0.6-3: static linking only

* **Choice:** every program links crt0, libaurora, libc and libm statically
  (about 100 KB each; Lua 440 KB).
* **Why:** roadmap decision; there is no dynamic loader.

## D-0.6-4: shell and coreutils written for Aurora

* **Choice:** `sh` (pipelines, redirection, lists, quoting, variables,
  globbing, builtins) and small utilities (`cat grep wc echo ls mkdir rmdir
  rm mv cp touch head kill sleep env chmod id uname pwd tee printf true
  false`) are original code.
* **Alternatives:** porting dash and sbase.
* **Why:** both alternatives expect a fuller POSIX (job control, `/bin/sh`
  paths, `wait3`, `sigsetjmp` everywhere) than 0.6 provides; the small
  originals exercise exactly what 0.6 implements and keep the image free of
  more third-party code. A ported interpreter (Lua) is the third-party
  exercise the roadmap asks for.

## D-0.6-5: the desktop session runs as root

* **Choice:** programs started from the Terminal run as uid 0; a second
  account (`user`, uid 1000) exists in the user table. Mode bits, owners
  and groups are stored on every file (persisted by PFS) and checked in
  open, exec, mkdir, unlink, rename, chmod and chown.
* **Why:** the roadmap's "single-user by default, but the checks are real".
  The acceptance test drops a child to uid 1000 with `setuid`. There are no
  set-user-ID files and no login.

## D-0.6-6: what stays out of 0.6

* **Shared memory:** `mmap(MAP_SHARED)` fails with ENOTSUP (the roadmap
  defers shared memory to 0.7a). Private file mappings are copies.
* **Job control:** no stopping or `fg`/`bg`; SIGSTOP, SIGTSTP, SIGTTIN and
  SIGTTOU are ignored by default instead of stopping a process (roadmap:
  job control deferred). Ctrl+C reaches the whole foreground process group.
* **Symbolic and hard links:** not supported (`link` fails with EMLINK,
  `symlink` with ENOSYS); the roadmap makes symlinks optional.
* **User page faults still take the BKL:** demand paging and copy-on-write
  are resolved under it (address spaces are BKL-protected). D-0.5-2's plan
  to move faults off the BKL is not done; it needs a per-address-space lock
  first.
