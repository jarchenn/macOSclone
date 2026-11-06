# Testing

Six layers: host, smoke, interaction, storage, network and SMP. Host tests do not replace guest tests. Every guest suite can run with N virtual CPUs (`AURORA_TEST_SMP=N`), and every guest wait fails on a lock-validator (`LOCKDEP`) report as well as on a panic.

## 1. Host unit tests: `make test`

`tests/host/test_*.c` compile the portable kernel modules (decoders, event
queues, rect math, VFS, ramfs, initramfs parser, path rules, command parser,
text buffer, heap core, page-allocator core, formatter, CRC) with the host C
compiler. They use AddressSanitizer + UndefinedBehaviorSanitizer when the
compiler supports them (llvm-mingw on Windows does).

| Suite | Checks | Covers |
|---|---|---|
| graphics | 42 | clipping incl. negative and near-INT32_MAX rects, framebuffer offsets with odd pitch and 24/32 bpp, overflow, surface size limits |
| input | 3701 | Shift/Caps/Ctrl, E0/E1 sequences, fake shift, ACK bytes vs 0xAA, mouse sign/overflow/resync/timeout, ring wraparound past 2^32 and overflow policy, event coalescing |
| fs | 134 | path normalisation rules, every VFS error code, offsets/gaps/truncate, RO mounts, file/total/handle limits with atomic replace, sorted listings, truncated/corrupted/oversized/traversal archives |
| apps | 101 | quoting/escapes, 200-char and 16-arg limits, text-buffer boundaries and the 16 KiB cap, selection and clipboard (replace, backward selection, all-or-nothing paste), formatter |
| memory | 2951 | heap alignment, coalescing, bounded growth over 1000 cycles, double/invalid/interior frees, limits and overflow; page allocator over a simulated map: unique pages, unaligned regions, invalid and double frees, reclaim, contiguous runs |

| acpi | 62 | RSDP/RSDT/XSDT/FADT/MADT/HPET parsing from synthetic tables: checksums, absurd lengths, zero-length and overlong MADT entries, CPU limit, ISO lookup, BIOS-area scan, damaged `\_S5` |
| elf | 30 | every ELF validation error: truncation, class, type, machine, phdr bounds, segment range, overlap, W+X, entry, size |
| net | 122 | IPv4/UDP/TCP/ICMP checksums and parsing, truncated and bad-length packets, DNS query building and answer parsing (compression, loops, CNAME, NXDOMAIN), DHCP options |
| pfs | 61 | format/mount/commit, a simulated power loss after every block write of a commit, torn superblocks, corrupt catalogs, refusing foreign disks, space limits |
| vfs_disk | 89 | the VFS on PFS: create/write/remove/rename persisted across remount, busy and non-empty errors |

Final run: **10/10 suites, 7297 checks, 0 failures** (ASan+UBSan on).

## 2. Real-guest smoke tests: `make smoke`

`tools/smoke_test.py` starts QEMU headless (`-display none`, TCG), captures
COM1 to a file, connects QMP on 127.0.0.1 with an ephemeral port, uses a fresh
temp directory per run and always kills its QEMU process.

| Test | Result |
|---|---|
| boot-uefi-128M | pass |
| boot-uefi-256M | pass (screenshot `desktop.png`) |
| boot-uefi-512M | pass |
| boot-bios-256M | pass |
| panic-diagnostic (panic build) | pass: vector 14, `error_code=…02`, `CR2=0`, kernel RIP, registers; guest still alive, no reset |

Each boot must print all `AURORA_MILESTONE` markers in order. The markers are
emitted after each stage has actually completed, not unconditionally at
start-up. A boot also fails on `AURORA_PANIC`, early QEMU exit or a timeout.

## 3. Guest interaction tests: `make integration`, `make soak`

`tools/interaction_test.py` boots the `test` build (`AURORA_TEST=1` adds `@@…`
serial markers: window geometry/focus, cursor position, clicks, terminal
output lines, Notes load/save with length + CRC-32, VFS results, heap stats).
Input is real: QMP `send-key` / `input-send-event` reach the guest through
QEMU's emulated PS/2 controller. The tests compute click targets from the
reported geometry and wait for observable markers, not fixed sleeps.

Final run: **23/23 pass**, and the soak (240 s, 1050 random actions) passes.
The 0.2+ scenarios cover user programs and faults, memory isolation,
preemption, crash containment, stdin/shell/Ctrl+C, process memory reclaim,
window resizing, clipboard and selection (keyboard and mouse), the Caps Lock
LED, and ACPI power-off (QEMU must exit through S5).
Per-scenario details are in `STATUS.md`. Results are written to
`build/test-results/interaction.json` and `soak.json`, along with serial logs
and screenshots.

Assertions are semantic: for example, a note is checked by comparing the
CRC-32 and length the guest reports with Python's CRC of the typed text. A
screenshot is never treated as a pass on its own.

## 4. Persistent storage: `make storage`

`tools/storage_test.py` boots with a fresh 32 MiB data disk per scenario. It
checks persistence across reboots, that a Notes save survives an immediate
QEMU kill, crash consistency (a guest stress loop writes numbered rounds
while the harness kills QEMU 4 times at random moments; after each kill
every file must be complete and every round the guest reported durable must
be present), that a disk with damaged superblocks is left untouched, and that
Files, Terminal and user programs see the disk state. Result: **5/5**.

## 5. Networking: `make network`

`tools/network_test.py` starts DNS, UDP-echo, TCP-echo and HTTP servers on
the host and drives the guest's programs against them through slirp. Result:
**9/9** (details in [networking.md](networking.md)).

## 6. SMP and ACPI: `make smp`, `make stress`

`tools/smp_test.py` (details in [smp.md](smp.md)): boots at 1, 2, 4 and 8
CPUs and checks every CPU's counters; checks that CPU-bound processes get
user time on all four CPUs; runs the TLB shootdown test with its control;
checks that QMP `system_powerdown` shuts down cleanly with a file that is
intact after reboot; and runs the `-smp 8` stress. Result: **7/7**, plus the
30-minute stress (see `STATUS.md`).

## 7. POSIX userland: `make posix`

`tools/posix_test.py` runs the 0.6 acceptance tests from the Terminal:
`posixtest` (printf/malloc/file I/O/mmap/8 pthreads), a `cat | grep | wc -l
>` pipeline checked against a host-side count, Ctrl+C under `sh`,
`proctest` (segfaulting child via waitpid, exec, posix_spawn, orphans,
pipes, poll, dup), `forktest 1000` (free page baseline), `sigtest`
(register-clobbering handlers, pending signals, EINTR, SA_RESTART),
`permtest` (uid 1000 vs a root 0600 file), VT100 rendering, `rentest`
across a reboot with a data disk, and Lua 5.4.9's test suite (portable
subset, `_U`). Result: **10/10 at -smp 1 and at -smp 4**.

## Manual checks

| Check | Procedure | Observed |
|---|---|---|
| GDB stub | `make debug`, then connect to `tcp:1234` | GDB remote `?` query answered `T05` (stopped) |
| Interactive window | `make run`, click the Terminal icon, type `help`, press Ctrl+Alt+G | not performed in this session (the same image and command pass headless) |
| Visual review | screenshots in `docs/screenshots/` | inspected: no cursor trails, no stale chrome, correct focus colours |

## Not covered

Linux/Docker builds (not executed), real hardware, kernel stack-overflow
guard page (mapped but not deliberately triggered), and the right mouse button
and wheel (not implemented).
