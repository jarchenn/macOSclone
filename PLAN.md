# Aurora OS — implementation plan (release 0.1)

Aurora OS is an original, experimental x86-64 operating system with a desktop
inspired by the restraint of classic desktop UIs. It is **not** macOS and has no
compatibility with Apple software or hardware.

## Platform decisions

| Area | Choice | Why |
|---|---|---|
| Guest | x86-64, 1 vCPU, QEMU `pc` machine, PS/2 keyboard + mouse, std VGA | Spec reference machine |
| Firmware | UEFI (QEMU-bundled edk2/OVMF) primary; BIOS via `limine bios-install` secondary | OVMF ships with QEMU on Windows and Linux |
| Bootloader | Limine **v12.9.3** (released 2026-10-06), binary release zip, SHA-256 pinned | Newest release older than two weeks |
| Protocol header | `limine.h` from limine-protocol commit `3a0526b` (2026-09-26) | Last protocol commit before v12.9.3 |
| Base revision | 6 (newest specified) — kernel verifies it was honoured | Strict, documented machine state |
| Compiler | Clang/LLD, `--target=x86_64-unknown-none-elf`, C11 | Single toolchain on Windows and Linux |
| Host tests | Same sources compiled for the host (llvm-mingw on Windows, system clang on Linux) with ASan/UBSan where available | Portable logic tests |
| Build | `Makefile` → `tools/build.py` (Python 3, stdlib only) | Works on Windows without a POSIX shell |
| Image | MBR disk image with one FAT16 partition (type 0xEF), built by `tools/mkfat.py` | No mtools/xorriso dependency |
| Font | Spleen 2.2.0 8x16 + 12x24 (BSD-2-Clause), converted at build time | Redistributable bitmap font |

## Kernel design summary

* Higher-half, non-PIE kernel at `0xffffffff80000000` (`-mcmodel=kernel`),
  general-purpose registers only (`-mgeneral-regs-only`), no red zone.
* Limine requests live between start/end markers in `.limine_requests`.
* Boot order: serial → validate Limine responses and copy everything needed
  → GDT/TSS (IST stack for #DF) → IDT/exception stubs → physical bitmap
  allocator → own page tables (kernel W^X, HHDM for usable RAM, framebuffer WC
  only if the PAT layout Limine documents is verified, guard pages, unmapped
  lower half) → reclaim bootloader memory → heap → PIC/PIT → PS/2 → VFS →
  desktop → event loop.
* Limine base revision ≥5 leaves the local APIC enabled with LINT0 masked,
  which would block legacy PIC interrupts. Release 0.1 disables the local APIC
  via `IA32_APIC_BASE` and uses the 8259 PIC directly; APIC support is roadmap.
* Single address space, built-in apps, cooperative event loop. **Not** a
  process scheduler; apps are not isolated.

## Phases and gates

A. Scaffold + boot → `AURORA_BOOT_OK` on serial, guest-drawn screenshot.
B. GDT/IDT/exceptions/panic, PMM, VMM, heap, PIT → self-checks, panic test
   build, boots at 128/256/512 MiB.
C. PS/2 keyboard + mouse, bounded raw-byte rings, race-free idle.
D. Compositor (backbuffer, damage rects), wallpaper, menu bar, dock, windows
   (focus, drag, close, minimize, maximize), cursor.
E. VFS (`/system` initramfs RO, `/home` ramfs RW), Terminal, Files, Notes,
   Settings/About.
F. Host tests, QEMU smoke + interaction tests via QMP, docs, `make dist`.

Test-only instrumentation (`@@` serial markers) is compiled in only for the
`test` build variant (`AURORA_TEST=1`).
