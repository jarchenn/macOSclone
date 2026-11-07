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

## D-0.7-1: the desktop leaves the kernel

* **Choice:** the window manager, compositor, menu bar, dock and every app
  are user processes. `init` starts `/system/bin/display` (the display
  server) and restarts it when it exits (at most five starts a minute);
  each app (`terminal`, `files`, `notes`, `settings`, `welcome`) is its own
  process that connects to the server. The kernel keeps the framebuffer
  driver, the diagnostic console (shown again when the server exits) and a
  power task (task 0) for restart and power-off.
* **Why:** the 0.7 prerequisites assume the 0.2 "desktop as a user-space
  service", which 0.2 had deferred; the acceptance test "killing the
  compositor leaves the kernel running and the session restartable" cannot
  pass while the desktop is kernel code. Most of the 0.1 desktop code moved
  as is (`git mv` into `user/gui`), so its behaviour and look carried over.

## D-0.7-2: scan-out by the kernel from a shared buffer

* **Choice:** the server composes into a shared-memory XRGB8888 buffer it
  attaches to `/dev/display`; `AURORA_DISP_PRESENT` copies damaged
  rectangles to the framebuffer, converting the pixel format there (the
  roadmap's "one internal format, converting only at scan-out"). The
  framebuffer itself is never mapped into user space.
* **Why:** no device memory in user page tables (no cache-type handling,
  no MMIO pages in the copy-on-write and release paths), and the copy is
  the same one the 0.1 kernel compositor already made. The cursor is a
  separate layer: the scene buffer is kept apart from the presented one, so
  moving the cursor copies two small rectangles and redraws it.

## D-0.7-3: local message sockets with descriptor passing

* **Choice:** a kernel IPC primitive of its own: connected, message-
  preserving sockets (`socketpair`, `listen`/`connect` on names in a small
  kernel namespace, `accept`), `sendmsg`/`recvmsg` carrying up to 64 KiB and
  8 descriptors per message. Names under `system.` can only be taken by
  root. Local sockets cannot be sent through local sockets.
* **Alternatives:** BSD `AF_UNIX` with paths in the VFS.
* **Why:** the display protocol needs message boundaries and descriptor
  passing (shared buffers), nothing more. Keeping names out of the VFS
  avoids socket nodes in PFS; refusing sockets-in-sockets avoids the
  reference cycles that Linux needs a garbage collector for.

## D-0.7-4: shared-memory objects

* **Choice:** `shm_create(size)` returns a descriptor; `mmap(MAP_SHARED)`
  of it maps the same pages in every process; `MAP_SHARED|MAP_ANONYMOUS`
  makes an unnamed object shared with children. Pages are allocated when
  the object is created (at most 64 MiB) and are never copy-on-write.
* **Why:** window buffers are written in full on every frame, so lazy
  allocation would only move the cost; eager pages keep the fault path
  simple (it maps the object's page). Page lifetime reuses the 0.6 share
  counts: the object holds one reference per page, each mapping one more.

## D-0.7-5: Bochs display adapter for mode setting

* **Choice:** runtime resolution changes through the Bochs DISPI registers
  of QEMU's standard VGA (`-vga std`), used only when its linear
  framebuffer is the one the bootloader reported. Twelve modes from 800x600
  to 2560x1440 are offered, filtered by video memory.
* **Alternatives:** virtio-gpu 2D.
* **Why:** the reference machine already uses `-vga std`; DISPI is a few
  port writes, while virtio-gpu needs a virtqueue driver and its own
  resource and scan-out management. Integer 2x scaling (optional in the
  roadmap) is not implemented.

## D-0.7-6: fonts

* **Choice:** stb_truetype 1.26 (public domain, pinned to a commit and a
  SHA-256) and DejaVu Sans, Sans Bold and Sans Mono 2.37 (Bitstream Vera /
  DejaVu licence, pinned SHA-256). Glyphs are rasterized once per font and
  size into a cache; anti-aliased coverage is blended in linear light;
  pair kerning comes from the font's `kern`/`GPOS` data (proportional faces
  only). The 0.1 Spleen bitmap fonts are the fallback when a font file
  cannot be loaded.
* **Why:** the roadmap's choice (stb_truetype first). Neither project
  publishes a digest next to the download, so the pinned digests are those
  of the files downloaded and inspected on 2026-11-06 (DEPENDENCIES.md).

## D-0.7-7: keymaps, repeat and text input in the display server

* **Choice:** `/dev/input` delivers raw scan codes; the server maps them
  with layout files from `/system/keymaps` (US and US-International with
  dead keys and an AltGr layer), repeats keys itself (configurable delay
  and rate; the keyboard's own repeats are dropped) and sends apps both key
  events and separate text events. The server also keeps the clipboard (its
  own copy, so content outlives the app that copied it).
* **Why:** the roadmap's "keymap files" and "text-input events separate
  from key events so an input method can plug in later". One process owns
  the keyboard state, so Caps Lock, dead keys and the LED stay consistent
  across apps.

## D-0.7-8: dialogs as sheets, apps single-instance

* **Choice:** the open and save dialogs are modal sheets drawn inside the
  app's window by the toolkit, not separate windows. Each app runs as one
  process with one main window; launching it again (dock, shortcut, Files
  opening a note) reaches the running instance.
* **Why:** a sheet needs no window-to-window parent protocol and keeps the
  dialog with its document. Single instances keep the 0.1 behaviour the
  interaction tests expect (one Notes, one Terminal).

## D-0.7-9: golden screenshots

* **Choice:** `make desktop` compares regions of QEMU screendumps with PNGs
  in `tests/golden/` (largest per-channel difference 8; at most 0.1 % of
  the pixels may differ). `--update-goldens` regenerates them; regenerated
  images are inspected by eye before they are committed.
* **Why:** rendering is deterministic under TCG, so the tolerance only
  absorbs differences in anti-aliasing should the code change slightly. The
  goldens are produced by Aurora itself; they show that rendering does not
  change unnoticed, not that it matches an external reference.
