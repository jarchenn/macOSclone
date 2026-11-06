# Status — Aurora OS 0.6.0

Status values: **implemented+tested** (emulator evidence), **implemented+host-tested**
(unit tests only), **implemented, manual**, **incomplete**, **not implemented**.
"Emulator" means the real kernel running in QEMU 11.1.0 (TCG, `pc` machine);
"host" means portable modules compiled into host unit tests with ASan+UBSan.
Results below are from the 0.6 run on 2026-11-06 (Windows 11 x86-64 host,
16 cores) on the code as committed. Every guest suite was run at `-smp 1`
and at `-smp 4` (`AURORA_TEST_SMP=4`) and fails on any lock-validator
(`LOCKDEP`) report. The 0.5 SMP stress was re-run on 0.6.

## Summary of the final run

| Suite | Command | Result |
|---|---|---|
| Host unit tests | `make test` | 10/10 suites, 7297 checks, 0 failures |
| Image checks | `make check` | 27/27 (incl. 45 user programs: static, non-PIE, W^X) |
| Boot smoke | `make smoke` | 5/5 at -smp 1 and 5/5 at -smp 4 |
| Guest interaction | `make integration` | 23/23 at -smp 1 and 23/23 at -smp 4 |
| Persistent storage | `make storage` | 5/5 at -smp 1 and 5/5 at -smp 4 |
| Networking | `make network` | 9/9 at -smp 1 and 9/9 at -smp 4 |
| SMP + ACPI | `make smp` | 8/8 (boot at 1/2/4/8, per-CPU work, TLB, power button, 120 s stress) |
| SMP stress | `make stress` | 1800 s at -smp 8: 507 fsstress runs, 57,839,250 bytes verified, 169 TCP transfers identical, 507 window cycles, 0 panics, 0 lockdep reports |
| POSIX userland | `make posix` | 10/10 at -smp 1 and 10/10 at -smp 4 (incl. the Lua 5.4.9 test suite) |
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
| Copy-on-write filesystem behind the VFS | implemented+tested | host `pfs` (61 checks: power loss after every block write, torn superblocks), host `vfs_disk` (89) |
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
| 30-minute stress at -smp 8 (CPU hogs, checksummed file writes, TCP to the host, window churn): no deadlock, no validator warning, all checksums match | implemented+tested | `stress` (1800 s): 525 `fsstress` runs, 59,893,050 bytes verified byte-for-byte and by CRC; 175 TCP transfers of 30,000 B identical; 525 window open/close cycles; 4 CPU hogs; 0 panics, 0 lockdep violations, 0 long holds (0.5 run). Re-run on 0.6: 507 runs, 57,839,250 bytes verified, 169 TCP transfers identical, 507 window cycles, 0 panics, 0 lockdep reports. A first 0.5 attempt failed on a test bug (it created a new directory per run until the 512-entry disk catalog was full); the test now reuses 24 directories |
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

## 0.6: POSIX-ish userland

| Criterion (roadmap acceptance test) | Status | Evidence |
|---|---|---|
| A cross-compiled program using printf, malloc, file I/O and pthread_create runs correctly | implemented+tested | `posix-c-program`: `posixtest` 17/17 (printf incl. `%e %g %.3f`, strtod, libm, 512 malloc blocks + 8 MiB, stdio and POSIX file I/O, mmap/mprotect, 8 threads x 20000 mutex increments, condvar producer/consumer, TLS + per-thread errno) |
| `cat file \| grep x \| wc -l > out` produces the right count | implemented+tested | `pipeline-count`: 30 lines, equal to the host's count of the same file; cat, grep and wc each exec'd |
| Ctrl-C kills the foreground program; the shell survives | implemented+tested | `ctrl-c-shell-survives`: `PTY_SIGNAL sig=2` to the shell's process group; spin exits with status 2, the same sh then runs `echo` |
| A child that segfaults is reported to its parent through waitpid status | implemented+tested | `segv-child-waitpid`: status 0xb (WIFSIGNALED, WTERMSIG = SIGSEGV), part of `proctest` 15/15 |
| 1,000 fork/exit cycles return the free-page count to its baseline | implemented+tested | `fork-1000`: 125364 -> 125364 pages (-smp 1), 125316 -> 125316 (-smp 4); each child dirties 4 copy-on-write pages |
| A signal handler that clobbers registers leaves the interrupted computation unchanged after sigreturn | implemented+tested | `sigreturn-registers`: 31 (-smp 1) / 199 (-smp 4) SIGUSR1 handlers from another process set every GPR, xmm0-8, xmm15 and the flags to garbage during an asm loop held in 13 GPRs + 8 xmm registers; the result equals the run with the signal blocked |
| A non-root process cannot open a root-owned file with mode 0600 | implemented+tested | `permissions`: `permtest` - after `setuid(1000)`: open for read/write, `access(R_OK)` EACCES; exec without x bit EACCES; chmod EPERM; create in a root 0755 dir EACCES; `setuid(0)` EPERM |
| rename and unlink results persist across a clean reboot | implemented+tested | `rename-unlink-reboot`: rename, rename over an existing file, unlink, mkdir+rename, rmdir, truncate; reboot via 8042; `rentest check` passed |
| A ported interpreter (Lua 5.4) builds and passes its test suite, or a documented subset | implemented+tested (subset) | `lua-test-suite`: Lua 5.4.9's `all.lua` with `_U=true` (the suite's own "portable, non-heavy" mode: skips `main.lua`, non-portable and memory-heavy parts) prints `final OK !!!` |
| Signals across CPUs (risk noted by the roadmap) | implemented+tested | the whole `make posix` suite passes at -smp 4 (10/10) |
| 0.6a ABI/toolchain: sysroot, argc/argv/envp/auxv, TLS via FS, FPU state | implemented+tested | `posixtest` TLS checks, sigtest xmm registers; `make check`: 45 user programs static ET_EXEC, W^X; `abi_check.c` compile-time ABI agreement |
| 0.6b libc + OS layer, mmap/munmap/mprotect, clock_gettime, libm | implemented+tested | newlib 4.6.0 (620 libc + 359 libm sources); posixtest |
| 0.6c posix_spawn native, fork COW, execve, waitpid, orphans to init, threads + futex | implemented+tested | proctest (exec + cloexec, posix_spawn dup2 action, orphan getppid() == 1), forktest, posixtest |
| 0.6d descriptor table, dup/dup2, cloexec, O_NONBLOCK, fcntl, poll/select | implemented+tested | proctest (shared offset, FD_CLOEXEC, F_DUPFD, EAGAIN, poll) |
| 0.6e pipes (EOF, EPIPE/SIGPIPE), signals, masks, EINTR | implemented+tested | proctest pipe checks (incl. PIPE_BUF atomicity), sigtest |
| 0.6f unlink, atomic rename, mkdir/rmdir, truncate, readdir, per-process cwd | implemented+tested | rentest, posixtest, `sh` `cd`; host `vfs_disk` (rename replace/EXDEV/EBUSY) |
| 0.6g uid/gid, mode bits enforced in open and exec | implemented+tested | permtest; host `vfs_disk` `metadata_persists` (mode/owner survive remount) |
| 0.6h pty + line discipline, VT100-subset Terminal, shell with pipelines/redirection, coreutils | implemented+tested | `vt100-terminal` (CR+CSI K, BS, CSI C/D, TAB); `stdin-shell-ctrl-c` (echo, Backspace erase, Ctrl+D EOF); pipeline-count |
| Symlinks (optional) | **not implemented** | `symlink` ENOSYS, `link` EMLINK |
| Shared memory, job control, dynamic linking, Linux ABI | **not implemented** (deferred by the roadmap) | `mmap(MAP_SHARED)` ENOTSUP (posixtest); stop signals ignored |
| User page faults off the BKL (open since 0.5) | **not done** | faults resolve under the BKL (D-0.6-6) |

System-call coverage: see the table in `docs/userland.md`.

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
* The Lua test suite passes in its portable subset (`_U`) only; the full
  suite (`main.lua`, heavy and non-portable parts) was not run.
* Files in `/home` and `/tmp` keep the 64 KiB size limit of 0.3 (programs
  and data in `/system` are not limited).
* The desktop session runs as root; there is no login and no set-user-ID.
* Without a data disk, `/home` is in RAM and resets on restart. The apps say
  which mode is active.
* US keyboard layout only. QEMU's i8042 buffers 16 bytes, so input injected
  faster than that is dropped by the emulator (the kernel's drop counters stay
  at 0).
