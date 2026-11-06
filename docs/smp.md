# SMP and ACPI (0.5)

Aurora runs on 1 to 16 CPUs; 1, 2, 4 and 8 are tested. This page describes
how the CPUs are found and started, how interrupts reach them, how the
kernel stays correct with several CPUs, and what is still serialized.

## ACPI tables (`drivers/acpi_tables.c`, portable)

The RSDP comes from the Limine handoff. If it is missing, Aurora scans the
first KiB of the EBDA and then 0xE0000–0xFFFFF. The parser checks the RSDP
checksum (and the extended checksum for revision ≥ 2), then the RSDT or XSDT
length and checksum, and then each table's. A table with a bad checksum is
ignored, not used. It reads:

* **FADT**: SCI IRQ, SMI command port, PM1a/b event and control blocks, the
  reset register (when the flags say it is supported) and the DSDT;
* **MADT**: the LAPIC address (including the 64-bit override entry), each
  processor's APIC ID and enabled/online-capable flags, I/O APICs, interrupt
  source overrides, LAPIC NMI pins. Entries with a zero length, entries that
  run past the table, and short or unsupported entries are skipped and
  counted;
* **HPET**: presence and base address (recorded, not used yet).

Host tests (`acpi` suite, 62 checks) build synthetic table sets in fake
physical memory: XSDT and RSDT paths, bad signatures and checksums, absurd
lengths, malformed MADT entries, more CPUs than supported, the BIOS-area
scan, and a damaged `\_S5`.

## Interrupt routing (`drivers/apic.c`)

Boot starts on the 8259 PIC (LAPIC in virtual-wire mode), as in 0.1–0.4.
Once the MADT is parsed:

1. The LAPIC is enabled (spurious vector 0xFF, LINT0 masked, LINT1 = NMI
   when the MADT says so). Every I/O APIC pin is masked.
2. Every IRQ that was enabled on the PIC is re-routed through the IOAPIC to
   the boot CPU. Interrupt source overrides are applied, so the PIT is IRQ0
   → GSI 2 and the SCI is level-triggered. Then the PIC is masked completely.
3. Each CPU's LAPIC timer is calibrated against PIT ticks (divide by 16) and
   runs periodically at 250 Hz. It drives that CPU's scheduler tick. The
   PIT keeps counting global time on the boot CPU.

All device IRQs go to CPU 0. Distributing them is deferred until the stress
suite has a longer track record (roadmap decision). Vectors: 0x20–0x2F ISA
IRQs, 0x40 LAPIC timer, 0x41 reschedule IPI, 0x42 TLB shootdown IPI, 0x43
halt IPI (panic), 0x80 system calls, 0xFF LAPIC spurious.

If there is no usable MADT/IOAPIC, or the CPU is in x2APIC mode (not
supported), Aurora stays on the PIC with one CPU.

## Per-CPU state (`arch/percpu.h`, `gdt.c`)

`struct cpu` holds the current task, the idle task, the time slice, the
reschedule flag, statistics, the locks held (for lockdep), and the CPU's own
GDT and TSS. Each CPU has its own RSP0 and IST stacks for #DF and NMI/#MC.
The IDT is shared.

The kernel reaches `struct cpu` through the GS base. While in the kernel,
`IA32_GS_BASE` points at it. In user mode it holds the user value (0), and
the kernel pointer waits in `IA32_KERNEL_GS_BASE`. `SWAPGS` runs on every
interrupt or system call from ring 3 and on every return to ring 3
(`interrupt_stubs.S`, `enter_user`). NMI, #DF and #MC decide by reading the
GS_BASE MSR instead of the saved CS, because they can arrive between the
ring change and the `SWAPGS`. FS/GS selectors are never reloaded in user
transitions, since a null selector load can clear the base.

## AP start-up (`arch/x86_64/smp.c`, `trampoline.S`)

* One page below 1 MiB is reserved at `pmm_init`, before anything else can
  take low memory. The trampoline is copied there.
* Temporary page tables identity-map 0–2 MiB and share the kernel half.
* For each enabled MADT processor: create its idle task (its stack is the
  AP's first stack), send INIT, wait 10 ms, then send SIPI and, if the AP has
  not set its "started" flag, a second SIPI 200 µs later. Then wait up to
  500 ms for it to come online.
* The AP goes from real mode to 32-bit protected mode, enables PAE, sets
  EFER.LME (+NXE), turns paging on with the temporary tables, enters long mode
  and jumps to `ap_entry64`. There it loads the kernel CR3, copies the boot
  CPU's CR0/CR4/EFER/PAT (so the framebuffer stays write-combining
  everywhere), loads its GDT/TSS/GS and the IDT, enables its LAPIC,
  calibrates its timer and enters its idle loop.
* An AP that does not respond is logged with the reason and skipped. Its
  slot is never reused, in case it wakes up late.

## Locks (`core/spinlock.c`)

* **Ticket spinlocks** (FIFO-fair) with `irqsave` variants.
* **Lock-order validator** (test build, `AURORA_LOCKDEP`). Each lock
  definition is a class. Taking lock B while holding lock A records the edge
  A → B, and an edge that would close a cycle is reported as an inversion.
  Recursive acquisition panics. Any hold longer than 200 ms is logged, and
  scheduling while holding a spinlock panics. Every guest test fails on any
  `LOCKDEP` line.
* **Lock order:** heap → vmm → pmm. `sched` is taken alone (wakeups may come
  from IRQ handlers, so it is always `irqsave`).
* **Big kernel lock (bridge).** Kernel code written before SMP runs under the
  BKL: system calls, the VFS and PFS, the drivers, the network stack, the
  desktop and its apps, and process creation and teardown. The BKL is
  recursive per task and is dropped automatically while a task is switched
  out. It is spun on with interrupts enabled, so IPIs are still answered.

Not under the BKL: user-mode code, the scheduler, timer ticks, IRQ handlers,
IPIs, the page allocator, the heap and kernel page-table updates. So user
programs run truly in parallel, and the kernel paths that every CPU needs
have their own locks. **The page-fault path still takes the BKL**: a user
fault kills the process, and teardown touches process tables. There is no
demand paging yet. Finer-grained locks for the VFS, network and desktop
come later (see roadmap).

## Scheduling (`core/sched.c`)

* One global task table under `sched_lock`, scanned round-robin from a
  shared cursor. Each CPU picks the first runnable task that no other CPU is
  running (`on_cpu`). That gives natural load balancing. There are **no
  per-CPU run queues**, which the roadmap suggests; this is a deliberate
  simplification for 0.5 (see the decision record).
* `sched_lock` is held across `context_switch` and released by the task that
  is switched to. So a task's registers are saved before any other CPU can
  pick it.
* Wakeups: waking a task that is still running (between checking its
  condition and blocking) sets `wake_pending`, so the next block returns at
  once. No wakeup is lost across CPUs. Waking a task while a CPU is idle
  sends that CPU a reschedule IPI.
* The kernel stays non-preemptible. The timer preempts only user mode and
  the idle task.
* Optional pinning (`task_create_pinned`) is used by diagnostics.

## TLB shootdown (`memory/tlb.c`)

Kernel unmaps that other CPUs may have cached (freeing a task's kernel
stack, the test page) send IPI 0x42 to every other online CPU and wait for
each to acknowledge. Only then are the pages freed. Senders are serialized,
and a sender spins with interrupts enabled. Shootdowns run with no spinlock
held, which is asserted, so no CPU can be stuck with interrupts off waiting
for a lock the sender holds. Since 0.6 user address spaces need them too:
a process's threads may run on several CPUs, so `memory/uvm.c` shoots down
`munmap`, `mprotect`, copy-on-write copies and fork's write-protection on
every CPU that has the address space loaded, before freeing any page. A
CR3 switch still flushes everything else (no global pages are used).

## ACPI power

* **S5 power-off**: `\_S5` is found by a byte scan of the DSDT. There is
  **no AML interpreter**, which the roadmap suggests (uACPI or LAI); see the
  decision record. Shut Down, `poweroff` and the power button all sync
  `/home` first.
* **Power button**: the fixed-feature event is enabled and the SCI routed
  (IRQ 9, level). QEMU's `system_powerdown` triggers an orderly shutdown
  (test `power-button`).
* **Reset**: the FADT reset register if present (QEMU's tables do not set
  it), then the 8042 reset line, then a triple fault.
* **Panic** sends a halt IPI to the other CPUs and prints which CPU
  panicked.

## Diagnostics

* `cpus` (Terminal): ticks, user/idle/kernel share, context switches, IPIs
  and TLB shootdowns received per CPU, BKL contention and lockdep counters.
* `tlbtest [noflush]` (test build): the shootdown test.
* `python tools/build.py run --smp 4` or `make run SMP=4`.

## Tests (`tools/smp_test.py`, `make smp`, `make stress`)

`boot-smp-{1,2,4,8}`, `user-on-every-cpu`, `tlb-shootdown`, `power-button`,
and `stress`: `-smp 8` with CPU hogs, parallel `fsstress` writers checking
every byte and CRC, TCP transfers to a host echo server, and window churn.
`AURORA_TEST_SMP=4` runs every older suite at 4 CPUs.
