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
