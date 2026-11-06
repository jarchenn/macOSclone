# Status — Aurora OS 0.4.0

Status values: **implemented+tested** (emulator evidence), **implemented+host-tested**
(unit tests only), **implemented, manual**, **incomplete**, **not implemented**.
"Emulator" means the real kernel running in QEMU 11.1.0 (TCG, `pc` machine,
1 CPU); "host" means portable modules compiled into host unit tests with
ASan+UBSan. Results below are from the clean run on 2026-11-06 (Windows 11
x86-64 host) after `make clean`. The Notes mouse-selection fix that came
after it was re-verified with the full interaction suite.

## Summary of the final run

| Suite | Command | Result |
|---|---|---|
| Host unit tests | `make test` | 9/9 suites, 7191 checks, 0 failures |
| Image checks | `make check` | 26/26 |
| Boot smoke | `make smoke` | 5/5 (UEFI 128/256/512 MiB, BIOS 256 MiB, panic diagnostic) |
| Guest interaction | `make integration` | 23/23 |
| Persistent storage | `make storage` | 5/5 |
| Networking | `make network` | 9/9 |
| Soak | `make soak` | 240 s, 1050 random actions, no panic or reset |

## Release 0.1 acceptance matrix

| # | Criterion | Status | Evidence |
|---|---|---|---|
| 1 | Boot via pinned Limine (12.9.3, base rev 6) | implemented+tested | smoke `boot-uefi-{128,256,512}M`, `boot-bios-256M` |
| 2 | Serial log, framebuffer diagnostic console, panic reporting | implemented+tested | smoke `panic-diagnostic` |
| 3 | IDT, exceptions, timer, keyboard, PS/2 mouse | implemented+tested | host `input` (3701 checks); every interaction scenario uses real PS/2 input via QMP |
| 4 | Page allocator, virtual layout, bounded heap | implemented+tested | host `memory` (2951 checks); `make check` W^X |
| 5 | Software desktop | implemented+tested | screenshots from the tests |
| 6 | Hit-testing, focus, drag, close/minimize/maximize | implemented+tested | `drag-overlap-focus`, `minimize-restore`, `launch-each-app` |
| 7 | Terminal, Files, Notes, Settings/About | implemented+tested | `launch-each-app`, `terminal-errors`, `notes-save-reopen-cat`, `theme-switch`, `menus` |
| 8 | Read-only initramfs + writable `/home` | implemented+tested | `readonly-save-keeps-edits`, host `fs` |
| 9 | Notes save → reopen → Files and Terminal read the same bytes | implemented+tested | `notes-save-reopen-cat` (104 B crc 336d4a95, 32 B crc 5e2fb704) |
| 10 | Reproducible build, tests, docs | implemented+tested on Windows; Linux path **not executed** | see "Not verified" |

## 0.2: user-process isolation

| Criterion | Status | Evidence |
|---|---|---|
| Ring-3 processes with their own page tables | implemented+tested | `process-memory-isolation`: two processes at the same VA 0x404000 see their own data; reads of kernel and HHDM addresses fault |
| Preemptive scheduling of user code | implemented+tested | `preemptive-concurrency`: a CPU-bound `spin` used 3356 ms of CPU while `counter` finished and the desktop opened Files |
| A crashing process does not crash the kernel | implemented+tested | `user-programs-and-faults` (NULL write, kernel read, stack overflow, #UD, #GP each kill only that process); `crash-does-not-stop-others` |
| Syscall pointer/length validation | implemented+tested | `badptr` (10/10 checks inside the guest) |
| ELF64 validation | implemented+host-tested | host `elf` (30 checks); the guest loads every `/system/bin` program |
| stdin, shell, Ctrl+C, kill | implemented+tested | `stdin-shell-ctrl-c` |
| Process memory reclaimed | implemented+tested | `process-memory-reclaimed`: free memory 239920 → 239920 KiB over 10 lifetimes |
| Desktop/apps as a user-space service | **not implemented** | apps remain kernel code (needs IPC + shared surfaces) |
| LAPIC timer / IOAPIC | **not implemented** | PIT + 8259 PIC (LAPIC in virtual-wire mode) |

## 0.3: persistent storage

| Criterion | Status | Evidence |
|---|---|---|
| VirtIO block driver with timeouts and error paths | implemented+tested | all storage scenarios; a timeout takes the disk offline (`EIO`) |
| Copy-on-write filesystem behind the VFS | implemented+tested | host `pfs` (61 checks: power loss after every block write, torn superblocks), host `vfs_disk` (49) |
| Saved content survives reboot | implemented+tested | `persist_across_reboot` (contents, mkdir, mv, rm) |
| Survives power loss | implemented+tested | `notes_save_survives_power_loss`; `crash_consistency_under_stress`: 4 hard QEMU kills mid-commit, every file complete, every round reported durable was present |
| Damaged disk not reformatted | implemented+tested | `damaged_disk_is_not_reformatted` |
| Delete and rename | implemented+tested | `persist_across_reboot`; host `fs`/`vfs_disk` |

The crash-consistency claim is limited to what was tested: QEMU process kills
with `cache=writeback`, plus the host model of a crash after every write.
Real disks that reorder writes across a FLUSH are not covered.

## 0.4: networking

| Criterion | Status | Evidence |
|---|---|---|
| virtio-net, ARP, IPv4 | implemented+tested | all network checks |
| DHCP | implemented+tested | `dhcp`: 10.0.2.15 / gw 10.0.2.2 / dns 10.0.2.3 |
| ICMP | implemented+tested | `icmp-ping` 3/3 |
| DNS | implemented+tested | `dns` (A after a CNAME, NXDOMAIN, literal) and a real lookup through slirp |
| UDP | implemented+tested | `udp-echo` 4/4 |
| TCP client | implemented+tested | `tcp-echo` 40000 B byte-identical; `kill-blocked-socket` |
| HTTP/1.1 client | implemented+tested | `http-fetch` 60000 B crc ed3095f1, 404 |
| TLS, TCP listen, congestion control, IPv6 | **not implemented** | `https://` is refused with a message |

## Later items

| Item | Status | Evidence |
|---|---|---|
| Window resizing | implemented+tested | `window-resize`: 600×450 → 480×370 → clamped to 320×180 → 600×450 |
| Clipboard + text selection | implemented+tested | `clipboard-copy-paste`: Shift+Home, Ctrl+A/C/X/V, double-click word, mouse drag, Terminal paste; host `apps` selection tests |
| Caps Lock LED | implemented+tested | `caps-lock-led` (LED command sent and cleared; letters follow) |
| ACPI power-off | implemented+tested | `acpi-poweroff`: QEMU exits 0.1 s after `poweroff`, through `\_S5` |
| Keyboard layouts, file dialogs, proportional fonts, graphics modes, audio, USB, SMP | not implemented | `docs/roadmap.md` |

## Not verified

* **Linux/Docker build**: the `Dockerfile` and `tools/setup.sh` were not run.
* **Interactive GTK window** (`make run`): same command as the tests apart
  from the display; not automated.
* **Real hardware**: untested and unsupported.
* **Kernel stack guard pages**: mapped, but no test triggers an overflow.

## Known limitations

* The desktop and its apps run in the kernel, so a bug there is a kernel
  panic. Only `/system/bin` programs are isolated.
* One CPU. The kernel is not preemptible (by design for now).
* User programs cannot use floating point (no FPU state saving).
* Without a data disk, `/home` is in RAM and resets on restart. The apps say
  which mode is active.
* US keyboard layout only. QEMU's i8042 buffers 16 bytes, so input injected
  faster than that is dropped by the emulator (the kernel's drop counters stay
  at 0).
