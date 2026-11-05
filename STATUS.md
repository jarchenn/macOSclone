# Status — Aurora OS 0.1.0

Status values: **implemented+tested** (emulator evidence), **implemented+host-tested**
(unit tests only), **implemented, manual**, **incomplete**, **blocked**.
"Emulator" means the real kernel running in QEMU 11.1.0 (TCG, `pc` machine);
"host" means portable modules compiled into host unit tests. Results below
are from the final clean run on 2026-11-04 (Windows 11 x86-64 host), after
`make clean`.

## Release 0.1 acceptance matrix

| # | Criterion | Status | Evidence |
|---|---|---|---|
| 1 | Boot via pinned Limine (12.9.3, base rev 6) | implemented+tested | smoke `boot-uefi-{128,256,512}M`, `boot-bios-256M` reach `AURORA_DESKTOP_READY`; milestones checked in order |
| 2 | Serial log, framebuffer diagnostic console, panic reporting | implemented+tested | serial logs in `build/test-results/*/serial.log`; smoke `panic-diagnostic` checks vector, error code, RIP, CR2, registers and halt-without-reset; `docs/screenshots/panic.png` |
| 3 | IDT, exceptions, timer, keyboard, PS/2 mouse | implemented+tested | boot fails if PIT ticks don't advance; integration scenarios inject real PS/2 keys and mouse packets via QMP; host `input` suite (3701 checks) |
| 4 | Page allocator, controlled virtual layout, bounded heap | implemented+tested | boot self-tests (pmm + heap); `make check` (W^X segments, higher half); host `memory` suite (2951 checks); heap limit 40/83/96 MiB at 128/256/512 MiB guest RAM |
| 5 | Software desktop: wallpaper, menu bar, dock, cursor, overlapping windows | implemented+tested | `docs/screenshots/desktop.png`, `apps-open.png`, `overlap.png` (QEMU screendump) |
| 6 | Hit-testing, focus, dragging, close/minimize/maximize, keyboard focus | implemented+tested | integration `drag-overlap-focus`, `minimize-restore` (no keys leak to minimized windows), `launch-each-app` |
| 7 | Terminal, Files, Notes, Settings/About | implemented+tested | integration `launch-each-app`, `terminal-errors`, `notes-save-reopen-cat`, `theme-switch`, `menus` |
| 8 | Read-only initramfs + writable RAM fs | implemented+tested | integration `readonly-save-keeps-edits`, `terminal-errors` (write/touch to /system → EROFS); host `fs` suite (malformed archives rejected) |
| 9 | Notes edit → save → close → reopen → Files and Terminal read | implemented+tested | integration `notes-save-reopen-cat`: two multiline notes with spaces and punctuation; CRC-32 and length match across Notes save, Files reopen and `cat` |
| 10 | Reproducible build, QEMU commands, smoke + interaction tests, docs | implemented+tested on Windows; Linux path written but **not executed** | `make` / `make test` / `make check` / `make smoke` / `make integration` / `make soak` / `make dist` all ran; see "Not verified" below |

## Required test scenarios (spec §11)

| # | Scenario | Result | Where |
|---|---|---|---|
| 1 | Boot at 128, 256, 512 MiB | pass (UEFI); plus BIOS at 256 MiB | smoke |
| 2 | Launch and focus each app from the dock | pass | `launch-each-app` |
| 3 | Drag, overlap, focus routing | pass | `drag-overlap-focus` (top window gets the click; keyboard follows focus; title bar can't go above the menu bar) |
| 4 | Minimize and restore | pass | `minimize-restore` (+ maximize/restore) |
| 5 | Multiline note save/close/reopen, byte comparison | pass | `notes-save-reopen-cat` (104 B crc 336d4a95, 32 B crc 5e2fb704) |
| 6 | Theme switch with multiple windows | pass | `theme-switch` (dark via Terminal, light via Settings) |
| 7 | Invalid command, missing file, write to /system | pass | `terminal-errors`, `readonly-save-keeps-edits` |
| 8 | Text-buffer and command-length limits | pass | `input-limits` (200-char line, 16384-byte note, 20000 B file refused by Notes, 64 KiB file cap) |
| 9 | Repeated open/close, memory counters | pass | `open-close-memory`: 12 cycles × 3 windows, heap in use 6343072 → 6343072 B, live allocations 20 → 20 |
| 10 | Idle/input soak, several minutes | pass | `make soak`: 240 s, 1095 random actions, no panic, no reset |
| 11 | Reboot resets RAM files | pass | `reboot-resets-ramfs` (8042 reset; file gone, seeded example back) |
| 12 | Deliberate exception in separate diagnostic build | pass | smoke `panic-diagnostic` |

Also passing: `unsaved-dialog-save`, `cursor-edges` (cursor clamped at all four
edges), `menus`, rapid input (52-key burst intact, 0 kernel-side drops).

## Feature details

| Area | Status | Notes |
|---|---|---|
| UEFI boot (OVMF) | implemented+tested | primary path |
| BIOS boot (SeaBIOS) | implemented+tested | `limine bios-install`, built from the release's `limine.c` |
| GDT/TSS with IST stacks | implemented+tested | #DF on IST1, NMI/#MC on IST2 |
| Guard page below kernel stack | implemented | mapped layout checked in logs; an overflow is not deliberately triggered by any test |
| NX / W^X kernel mappings | implemented+tested | `make check` checks segments; NX enabled at runtime per log |
| Framebuffer write-combining | implemented+tested | used only after verifying PAT entry 5 = WC; log shows `write-combining` |
| LAPIC virtual wire for PIC IRQs | implemented+tested | required by Limine base rev ≥ 5; boot aborts if the timer is silent |
| Keyboard: Shift, Caps, Ctrl, extended keys, Backspace, arrows | implemented+tested | host decoder tests and guest typing (upper case, punctuation) |
| Mouse: move, left click, drag | implemented+tested | right button and wheel: not implemented |
| Window resize by dragging | not implemented | fixed sizes; maximize swaps the size |
| Terminal command set (spec table) + cd/pwd/mkdir | implemented+tested | `help` text lists them; not a POSIX shell |
| Files: navigate, RO/RW indication, open in Notes, auto refresh | implemented+tested | refreshes on VFS generation change |
| Notes: new/open/save/save as, unsaved dialog, capacity error | implemented+tested | character wrap (not word wrap) |
| Settings: theme, real system info, shortcuts, limitations | implemented+tested | `about.png` |
| Interactive GUI launch (`make run`) | implemented, manual | same command as the tests except `-display`/`-serial stdio`; not exercised by automation |
| GDB stub (`make debug`) | implemented, manually checked | a GDB remote-protocol `?` query to `tcp:1234` returned `T05` |
| Dockerfile / Linux build | **not verified** | no Docker or WSL on the dev host; see HANDOFF.md |
| Real hardware | not supported | untested |

## Known limitations (by design for 0.1)

* Single address space: apps aren't isolated, and an app fault halts the
  kernel. There is no scheduler or user mode.
* **Files are stored in memory and reset when Aurora restarts.**
* No networking, audio, USB, SMP, ACPI power-off or persistent storage.
* US keyboard layout only; keyboard LEDs aren't driven.
* QEMU's emulated i8042 buffers only 16 bytes, so input injected faster than
  that is dropped by the emulator itself (the kernel's own drop counters stay
  at 0).
