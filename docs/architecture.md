# Architecture

Aurora 0.1 is a single-address-space, single-CPU x86-64 kernel. The desktop
and its four applications are compiled into the kernel and driven by one
cooperative event loop. **This is not a process scheduler**: there are no
processes, no ring 3, no isolation. A fault in any app is a kernel fault.

```
 Limine (UEFI or BIOS) ──► _start (entry.S) ──► kmain_early (Limine stack)
        serial ─► boot_collect (validate + copy Limine responses)
        fb_init ─► diagnostic console ─► cpu_detect ─► GDT/TSS ─► IDT
        pmm_init (bitmap) ─► vmm_init (own page tables, CR3 switch)
   switch_stack_and_call(guarded kernel stack) ──► kmain_stage2
        reclaim bootloader memory ─► heap ─► self-tests
        PIC remap + LAPIC virtual wire ─► PIT 250 Hz ─► PS/2 ─► sti
        VFS: /system (initramfs, RO) + /home (ramfs, RW)
        AURORA_BOOT_OK ─► desktop_main (never returns)
```

## Source layout

| Directory | Contents |
|---|---|
| `kernel/arch/x86_64/` | entry, GDT/TSS, IDT, interrupt stubs + dispatch, page tables, CPUID, port I/O |
| `kernel/core/` | boot-info validation, logging/test markers, panic, event queues, main |
| `kernel/memory/` | bitmap physical allocator, virtual layout, heap core + kernel heap |
| `kernel/drivers/` | serial, framebuffer, PIC/LAPIC virtual wire, PIT, PS/2 controller, keyboard & mouse decoders |
| `kernel/fs/` | path rules, VFS core + open-file table, ramfs and read-only backends, initramfs parser |
| `kernel/graphics/` | rect math, surfaces + primitives, compositor, diagnostic console |
| `kernel/desktop/` | theme/wallpaper/icons, window manager, widgets, desktop shell (menus, dock, input routing, loop) |
| `kernel/apps/` | Terminal (+ command parser), Files, Notes (+ text buffer), Settings/About, Welcome |
| `tools/` | build driver, image writer, initramfs packer, asset generator, QEMU harness and tests |
| `tests/host/` | host-side unit tests for the portable modules |

Modules marked portable (decoders, event queues, rect math, VFS, initramfs,
cmdline, textbuf, heap core, pmm core, formatter) contain no hardware access
and are compiled unchanged into the host tests.

## Interrupt flow

1. The CPU vectors through a 16-byte stub (`interrupt_stubs.S`). Stubs for
   vectors without a CPU error code push a dummy 0, so every frame looks
   alike: `[r15..rax][vector][error][rip cs rflags rsp ss]`.
2. `isr_common` saves all GPRs, clears DF, re-aligns RSP to 16 bytes and
   calls `interrupt_dispatch(frame)`.
3. Vectors 0–31 → `panic_exception` (fatal in 0.1). #DF runs on IST1 and
   NMI/#MC on IST2, so a stack overflow into the guard page still reports.
4. Vectors 0x20–0x2F → spurious check (IRQ7/15 ISR read) → registered
   handler → EOI (slave then master for IRQ ≥ 8).

All IDT entries are interrupt gates, so IF is clear inside handlers. Handlers
are short: PIT increments a 64-bit tick counter; PS/2 reads bytes and pushes
them to bounded rings. No allocation, rendering or file I/O in IRQ context.

## Event handling

```
IRQ1/IRQ12 ─► byte_ring (512 B each, SPSC, drop-newest on overflow)
main loop  ─► kbd/mouse decoders ─► event_queue (256 events, motion coalesced)
           ─► desktop: shortcuts, menus, dock, window hit-testing, focus,
              drag capture ─► focused window's key()/mouse() callbacks
           ─► app ticks (250 ms) ─► wm_render_dirty ─► compositor_frame
           ─► cli; if (no work) sti;hlt else sti   (race-free idle)
```

`sti; hlt` executes atomically (STI's interrupt shadow), so an interrupt that
arrives after the "no work" check always wakes the halt.

## Memory ownership

* Boot data: every Limine response is validated and copied into `g_boot`
  (kernel .bss) before the page-table switch; afterwards bootloader-reclaimable
  memory (Limine's page tables, GDT, stack, responses) is released to the page
  allocator. The initramfs module stays in "kernel and modules" memory and the
  read-only VFS nodes point into it.
* Physical pages: owned by the bitmap allocator; page tables, the kernel stack
  and heap pages come from it.
* Heap: windows, surfaces, VFS nodes and RAM file data. App state is static
  (one instance per app) so closing a window frees its surface only;
  the integration test checks heap usage returns to the same value after
  repeated open/close cycles.

See [memory.md](memory.md) for the address-space layout.

## Compositor and app model

The compositor owns a screen-sized XRGB8888 backbuffer and a list of up to 16
damage rectangles (merged when overlapping; collapsed to one bounding box when
full). Each frame recomposes only the damaged areas, back to front:

wallpaper (pre-rendered surface) → windows bottom-to-top (shadow, title bar,
controls, content surface with rounded bottom corners, border) → menu bar →
dock (+ tooltip) → open menu → cursor.

The cursor is always drawn last and erased by recomposing its old rectangle,
so it never leaves trails. Presentation converts XRGB to the framebuffer's
reported channel layout (fast `memcpy` path for the common R16/G8/B0 32-bpp
layout) using the reported pitch.

Each window owns a content surface. Apps never touch the framebuffer: they
draw into their surface in `render()` when marked dirty, and the WM blits it.
Windows are referenced by stable ids; callers re-validate with `wm_get(id)`,
which avoids callbacks touching destroyed windows.

## Storage model

`/system` is the read-only initramfs; `/home` is a RAM filesystem seeded from
the archive's `home/` entries at boot. **Files are stored in memory and reset
when Aurora restarts.** See [filesystem.md](filesystem.md).
