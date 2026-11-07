# Architecture

Aurora 0.7 is an SMP x86-64 kernel (1–16 CPUs; see [smp.md](smp.md)) with preemptively scheduled,
isolated user processes (ring 3, one address space each; see
[processes.md](processes.md), [userland.md](userland.md)). Since 0.7 the
desktop is user space too: a display server and one process per app,
talking over local sockets and shared memory ([desktop.md](desktop.md)). A
fault in any of them kills only that process; init restarts the display
server.

```
 Limine (UEFI or BIOS) ──► _start (entry.S) ──► kmain_early (Limine stack)
        serial ─► boot_collect (validate + copy Limine responses)
        fb_init ─► diagnostic console ─► cpu_detect ─► GDT/TSS ─► IDT
        pmm_init (bitmap) ─► vmm_init (own page tables, CR3 switch)
   switch_stack_and_call(guarded kernel stack) ──► kmain_stage2
        reclaim bootloader memory ─► heap ─► self-tests
        scheduler (task 0 = this stack, idle task) ─► BKL
        PIC remap + LAPIC virtual wire ─► PIT 250 Hz ─► PS/2 ─► sti
        ACPI tables ─► LAPIC + IOAPIC (PIC masked) ─► LAPIC timer
        ─► start APs (INIT-SIPI-SIPI) ─► SCI/power button ─► PCI scan
        VFS: /system (initramfs, RO) + /home (ramfs, RW)
        virtio-blk found? ─► /home moves onto PFS on the data disk
        virtio-net found? ─► network task (DHCP)
        init (pid 1) ─► AURORA_BOOT_OK ─► power_main (task 0: restart/power-off)
        init ─► /system/bin/display (restarted if it exits) ─► welcome, apps
```

## Source layout

| Directory | Contents |
|---|---|
| `kernel/arch/x86_64/` | entry, GDT/TSS, IDT, interrupt stubs + dispatch, page tables, CPUID, port I/O |
| `kernel/core/` | boot-info validation, logging/test markers, panic, event queues, scheduler, processes, syscalls, ELF validation, main |
| `kernel/net/` | protocol parsing (portable), ARP/IPv4/ICMP/UDP/DHCP/DNS stack, TCP |
| `kernel/include/abi/` | the user/kernel ABI shared with `user/` |
| `user/libc/` | Aurora's libc layer over newlib: crt0, start-up, system-call wrappers, pthreads, locks, posix_spawn, termios; header overrides (0.6) |
| `user/bin/` | the programs in `/system/bin`: shell, utilities, tests, network tools |
| `tools/userland.py` | builds the sysroot (newlib, libc layer) and Lua (0.6) |
| `kernel/memory/` | bitmap physical allocator, virtual layout, heap core + kernel heap |
| `kernel/drivers/` | serial, framebuffer, PIC, LAPIC + IOAPIC, PIT, PS/2 controller (+ Caps Lock LED), keyboard & mouse decoders, ACPI tables (portable parser) + power, PCI, VirtIO core, virtio-blk, virtio-net |
| `kernel/fs/` | path rules, VFS core + open-file table, ramfs and read-only backends, initramfs parser, PFS (persistent, copy-on-write) |
| `kernel/graphics/` | rect math, surfaces + primitives (shared with user space), diagnostic console |
| `user/gui/` | 0.7 desktop: display server, client library + toolkit + dialogs, TrueType text, apps (see [desktop.md](desktop.md)) |
| `tools/` | build driver, image writer, initramfs packer, asset generator, QEMU harness and tests |
| `tests/host/` | host-side unit tests for the portable modules |

Modules marked portable (decoders, event queues, rect math, VFS, initramfs,
cmdline, textbuf, heap core, pmm core, formatter, ELF validator,
PFS, network protocol parsing) contain no hardware access
and are compiled unchanged into the host tests.

## Interrupt flow

1. The CPU vectors through a 16-byte stub (`interrupt_stubs.S`). Stubs for
   vectors without a CPU error code push a dummy 0, so every frame looks
   alike: `[r15..rax][vector][error][rip cs rflags rsp ss]`.
2. `isr_common` saves all GPRs, clears DF, re-aligns RSP to 16 bytes and
   calls `interrupt_dispatch(frame)`.
3. Vectors 0–31: taken in user mode (CS RPL 3) → page faults inside a
   region are resolved (demand paging, copy-on-write); anything else becomes
   a signal for the thread (`process_user_exception`); taken in kernel mode → `panic_exception`. #DF runs on IST1 and
   NMI/#MC on IST2 (per CPU), so a stack overflow into the guard page still
   reports. Entries from ring 3 execute SWAPGS first (per-CPU data).
   Vector 0x80 (DPL 3 gate) → `syscall_dispatch`.
4. Vectors 0x20–0x2F (PIC until the IOAPIC takes over, then IOAPIC → LAPIC
   EOI; 0x40–0x43 are LAPIC timer and IPIs) → spurious check (IRQ7/15 ISR read) → registered
   handler → EOI (slave then master for IRQ ≥ 8).

All IDT entries are interrupt gates, so IF is clear inside handlers (the
syscall handler re-enables interrupts). Handlers are short: PIT increments a
64-bit tick counter and calls `sched_tick`; PS/2 reads bytes, pushes them to
bounded rings and wakes `/dev/input` readers. No allocation, rendering or file I/O
in IRQ context. After EOI, a return to user mode (or idle) may switch tasks,
and every return to user mode runs `signal_return_to_user` (signal
delivery, thread exit).

## Event handling

```
IRQ1/IRQ12 ─► byte_ring (512 B each, SPSC, drop-newest on overflow) ─► wake
/dev/input read (display server) ─► scan codes / mouse packets decoded
           ─► keymap, repeat, shortcuts, menus, dock, hit-testing, focus
           ─► S_KEY / S_TEXT / S_MOUSE to the focused or hovered app
app ─► redraw into a free shared buffer ─► C_COMMIT ─► damage ─► compose
     ─► /dev/display PRESENT (kernel copies to the framebuffer)
```

## Memory ownership

* Boot data: every Limine response is validated and copied into `g_boot`
  (kernel .bss) before the page-table switch; afterwards bootloader-reclaimable
  memory (Limine's page tables, GDT, stack, responses) is released to the page
  allocator. The initramfs module stays in "kernel and modules" memory and the
  read-only VFS nodes point into it.
* Physical pages: owned by the bitmap allocator; page tables, the kernel stack
  and heap pages come from it.
* Kernel heap: VFS nodes, RAM file data, open-file descriptions, socket
  queues and message buffers, shared-memory page lists. Window buffers are
  shared-memory pages, freed when the last mapping and descriptor go; the
  integration test checks the kernel heap returns to the same value after
  repeated app open/close cycles.

See [memory.md](memory.md) for the address-space layout.

## Compositor and app model

See [desktop.md](desktop.md). In short: the display server keeps a scene
buffer and a presented buffer (scene plus cursor), recomposes only damaged
rectangles, and draws decorations around each app's committed shared
buffer. Apps draw into one of two buffers while the server shows the other
(explicit attach, commit and release).

## Storage model

`/system` is the read-only initramfs. `/home` lives on PFS on the VirtIO data
disk when one is attached, and survives restarts. Otherwise it is a RAM
filesystem seeded from the archive and **reset when Aurora restarts**. See
[filesystem.md](filesystem.md) and [networking.md](networking.md).
