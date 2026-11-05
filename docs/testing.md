# Testing

Three layers. Host tests do not replace guest tests.

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
| apps | 70 | quoting/escapes, 200-char and 16-arg limits, text-buffer boundaries and the 16 KiB cap, formatter |
| memory | 2951 | heap alignment, coalescing, bounded growth over 1000 cycles, double/invalid/interior frees, limits and overflow; page allocator over a simulated map: unique pages, unaligned regions, invalid and double frees, reclaim, contiguous runs |

Final run: **5/5 suites, 6898 checks, 0 failures** (ASan+UBSan on).

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

Final run: **13/13 pass**, and the soak (240 s, 1095 random actions) passes.
Per-scenario details are in `STATUS.md`. Results are written to
`build/test-results/interaction.json` and `soak.json`, along with serial logs
and screenshots.

Assertions are semantic: for example, a note is checked by comparing the
CRC-32 and length the guest reports with Python's CRC of the typed text. A
screenshot is never treated as a pass on its own.

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
