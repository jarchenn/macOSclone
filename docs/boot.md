# Boot

## Image layout (`build/aurora.img`)

Raw disk image written by `tools/mkfat.py` (pure Python, reproducible
timestamps):

* LBA 0: MBR with one partition (type 0xEF, active). `limine bios-install`
  later writes Limine's BIOS stage 1 here and stage 2 into the gap.
* LBA 2048…: 64 MiB FAT16 filesystem with VFAT long names:
  * `/EFI/BOOT/BOOTX64.EFI` (Limine UEFI, removable-media path)
  * `/boot/limine/limine.conf`, `/boot/limine/limine-bios.sys`
  * `/boot/aurora/kernel.elf`, `/boot/aurora/initramfs.img`

Both firmware paths are tested: UEFI with QEMU's edk2/OVMF, and BIOS with
SeaBIOS (see testing.md).

## Limine configuration

`boot/limine.conf` (Limine 12.x syntax): one entry using `protocol: limine`,
`path: boot():/boot/aurora/kernel.elf`, the initramfs as
`module_path` with `module_string: initramfs`, and a requested
`resolution: 1024x768x32`. The kernel never assumes the request was honoured;
it reads the actual geometry from the framebuffer response.

## Protocol use

The kernel requests base revision **6** (newest in the pinned header) and
panics if Limine does not acknowledge it (third tag word not zeroed). Requests,
all between the start/end markers in section `.limine_requests`:

| Request | Required | Validation |
|---|---|---|
| HHDM | yes | offset ≥ 0xffff800000000000, page aligned |
| Executable address | yes | physical base page aligned, virtual base = 0xffffffff80000000 |
| Memory map | yes | 1..256 entries, non-NULL, no 64-bit overflow; overlaps logged |
| Framebuffer | for the desktop | RGB memory model; 24/32 bpp with 8-bit channels; pitch ≥ width·Bpp; ≤ 8192×8192 |
| Module | for /system | located by `module_string`; archive fully validated by the parser |
| Paging mode | no | must report 4-level if present |
| Bootloader info, firmware type | no | strings sanitised to printable ASCII |

## Machine state assumptions (base revision 6, x86-64)

Entered at `_start` in long mode, paging on, IF/DF clear, CS=0x28, data
segments 0x30, RSP at the top of a ≥64 KiB stack in bootloader-reclaimable
memory with a 0 return address pushed (so the SysV alignment rule already
holds), all GPRs 0, IDT empty, legacy PICs masked.

Important consequence: with base revision ≥ 5 Limine leaves the **local APIC
enabled with LINT0 masked**. On the reference machine the 8259 PIC is wired to
LINT0, so without action no IRQ would ever arrive. Aurora maps the LAPIC page
(uncached) and programs LINT0 for ExtINT delivery ("virtual wire mode"); in
x2APIC mode it writes MSR 0x835 instead. The boot fails loudly
(`timer: PIT interrupts are not arriving`) if ticks do not advance.

## Kernel bring-up order

serial → validate/copy boot info → framebuffer + diagnostic console → CPUID →
own per-CPU GDT (kernel 0x08/0x10, user 0x18/0x20, TSS 0x28 with IST stacks; GS = per-CPU data) → IDT → physical
allocator → own page tables + CR3 switch → switch to the guarded kernel stack →
reclaim bootloader memory → heap → self-tests → PIC remap + LAPIC virtual wire
→ PIT → PS/2 → `sti` → timer check → ACPI → LAPIC/IOAPIC → LAPIC timer →
APs → SCI → VFS → PCI/virtio → network → `AURORA_BOOT_OK` → desktop.

Each stage prints an `AURORA_MILESTONE <name>` line on COM1 after it actually
completed; the smoke test checks they appear, in order.

## Diagnostics

* COM1 115200 8N1 (`-serial stdio` in `make run`, a file in tests).
* Until the desktop starts, log lines are mirrored to a framebuffer console.
* Panics (`panic()` or any CPU exception) print vector name, error code, RIP,
  CS, RFLAGS, CR2 with decoded page-fault bits, all GPRs, CR0/CR3/CR4 and a
  frame-pointer backtrace to serial and to a red framebuffer screen, then halt
  with interrupts disabled. The panic path uses a static buffer, no heap, and
  guards against recursion.
* The `panic` build variant dereferences NULL after interrupts are up to
  exercise this path (`make smoke` checks the report).
