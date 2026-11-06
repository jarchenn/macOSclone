# Status — Aurora OS 0.5.0

Status values: **implemented+tested** (emulator evidence), **implemented+host-tested**
(unit tests only), **implemented, manual**, **incomplete**, **not implemented**.
"Emulator" means the real kernel running in QEMU 11.1.0 (TCG, `pc` machine);
"host" means portable modules compiled into host unit tests with ASan+UBSan.
Results below are from the 0.5 run on 2026-11-06 (Windows 11 x86-64 host,
16 cores). Every 0.1–0.4 guest suite was run twice: at `-smp 1` and at
`-smp 4` (`AURORA_TEST_SMP=4`). Every guest suite fails on any lock-validator
(`LOCKDEP`) report.

## Summary of the final run

| Suite | Command | Result |
|---|---|---|
| Host unit tests | `make test` | 10/10 suites, 7257 checks, 0 failures |
| Image checks | `make check` | 26/26 |
| Boot smoke | `make smoke` | 5/5 at -smp 1 and 5/5 at -smp 4 |
| Guest interaction | `make integration` | 23/23 at -smp 1 and 23/23 at -smp 4 |
| Persistent storage | `make storage` | 5/5 at -smp 1 and 5/5 at -smp 4 |
| Networking | `make network` | 9/9 at -smp 1 and 9/9 at -smp 4 |
| SMP + ACPI | `make smp` | 7/7 (boot at 1/2/4/8, per-CPU work, TLB, power button) |
| SMP stress | `make stress` | 1800 s at -smp 8: pass (details in the 0.5 table) |
| Soak | `make soak` | 240 s at -smp 4, 1061 random actions, no panic or reset |

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
| LAPIC timer / IOAPIC | implemented+tested (0.5) | see the 0.5 table |

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

## 0.5: SMP and ACPI

| Criterion (roadmap acceptance test) | Status | Evidence |
|---|---|---|
| Boots to the desktop with -smp 1, 2, 4 and 8 and reports the CPU count | implemented+tested | `boot-smp-{1,2,4,8}`: `@@SMP cpus=N`, every CPU's LAPIC timer advancing |
| Per-CPU counters show user processes running on every CPU | implemented+tested | `user-on-every-cpu`: 6 CPU-bound processes gained 358/409/411/405 user ticks on CPUs 0–3 |
| 30-minute stress at -smp 8 (CPU hogs, checksummed file writes, TCP to the host, window churn): no deadlock, no validator warning, all checksums match | implemented+tested | `stress` (1800 s): 525 `fsstress` runs, 59,893,050 bytes verified byte-for-byte and by CRC; 175 TCP transfers of 30,000 B identical; 525 window open/close cycles; 4 CPU hogs; 0 panics, 0 lockdep violations, 0 long holds. A first attempt failed on a test bug (it created a new directory per run until the 512-entry disk catalog was full); the test now reuses 24 directories |
| TLB test: a page unmapped on one CPU faults on another that just read it | implemented+tested | `tlb-shootdown`: 3/3 faults; the control without the IPI reads the stale page, so the test can detect a missing shootdown |
| Shutdown exits QEMU cleanly; saved files intact on the next boot | implemented+tested | `acpi-poweroff`, `power-button` |
| QEMU system_powerdown triggers the same clean shutdown | implemented+tested | `power-button`: SCI → sync → S5, file intact after reboot |
| ACPI reset reboots | **partial** | the FADT reset register is implemented, but QEMU's tables do not advertise one, so the guest uses the 8042 fallback (`reboot-resets-ramfs`); the register path itself is untested |
| All 0.1–0.4 suites pass at -smp 4 | implemented+tested | see the summary table |
| ACPI table parsing (RSDP/RSDT/XSDT/MADT/FADT/HPET), checksums | implemented+host-tested | host `acpi` (62 checks); UEFI (XSDT) and BIOS (RSDT) paths both boot |
| Per-CPU GDT/TSS/IST, GS-based per-CPU data | implemented+tested | all suites at -smp 4/8 (ring transitions with SWAPGS) |
| Lock-order validator, hold-time watchdog, sleep-with-spinlock check | implemented+tested | test build; 0 reports across every suite |
| Scheduler, allocator, heap, VMM free of the BKL | implemented+tested | own spinlocks (`sched`, `pmm`, `heap`, `vmm`) |
| User page-fault path free of the BKL | **not done** | a user fault kills the process and teardown takes the BKL (decision D-0.5-2) |
| AML interpreter (uACPI/LAI) | **not done** | `\_S5` byte scan (decision D-0.5-1) |
| Per-CPU run queues with load balancing | **not done** | one global queue with `on_cpu` ownership (decision D-0.5-3) |

## Later items

| Item | Status | Evidence |
|---|---|---|
| Window resizing | implemented+tested | `window-resize`: 600×450 → 480×370 → clamped to 320×180 → 600×450 |
| Clipboard + text selection | implemented+tested | `clipboard-copy-paste`: Shift+Home, Ctrl+A/C/X/V, double-click word, mouse drag, Terminal paste; host `apps` selection tests |
| Caps Lock LED | implemented+tested | `caps-lock-led` (LED command sent and cleared; letters follow) |
| ACPI power-off | implemented+tested | `acpi-poweroff`: QEMU exits 0.1 s after `poweroff`, through `\_S5` |
| SMP | implemented+tested (0.5) | see above |
| Keyboard layouts, file dialogs, proportional fonts, graphics modes, audio, USB | not implemented | `docs/roadmap.md` (0.7/0.8) |

## Not verified

* **Linux/Docker build**: the `Dockerfile` and `tools/setup.sh` were not run.
* **Interactive GTK window** (`make run`): same command as the tests apart
  from the display; not automated.
* **Real hardware**: untested and unsupported.
* **Kernel stack guard pages**: mapped, but no test triggers an overflow.

## Known limitations

* The desktop and its apps run in the kernel, so a bug there is a kernel
  panic. Only `/system/bin` programs are isolated.
* Up to 16 CPUs, but most kernel code runs under the big kernel lock, so
  system calls, file and network work are serialized. The kernel is not
  preemptible (by design for now).
* User programs cannot use floating point (no FPU state saving).
* Without a data disk, `/home` is in RAM and resets on restart. The apps say
  which mode is active.
* US keyboard layout only. QEMU's i8042 buffers 16 bytes, so input injected
  faster than that is dropped by the emulator (the kernel's drop counters stay
  at 0).
