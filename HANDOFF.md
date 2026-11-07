# Handoff

## State

Releases 0.1–0.6 are implemented, plus these roadmap "Later" items: window
resizing, delete and rename, a text clipboard, the Caps Lock LED and ACPI
power-off. 0.5 (SMP and ACPI) and 0.6 (POSIX-ish userland: newlib, fork/exec,
threads, signals, pipes, pty, shell, Lua) follow the user's "OS Roadmap: 0.5
and Beyond". `STATUS.md` has the evidence for each, including the items that
are not done. Work since 0.1 is on branch `roadmap-0.2-0.4`. `main` (pushed to `origin`, github.com/jarchenn/macOSclone)
holds 0.1. `build/` and `third_party/` are ignored; `make deps` re-downloads and
verifies Limine, newlib and Lua (pinned digests in `tools/fetch_deps.py`).

## Reproduce everything

```bash
make                     # release image -> build/aurora.img
make test                # host unit tests (10 suites)
make check               # ELF/image checks
make smoke               # boots: UEFI 128/256/512 MiB, BIOS 256 MiB, panic diagnostic
make integration         # 23 QMP-driven guest scenarios (test build)
make storage             # data-disk persistence + crash consistency (5 scenarios)
make network             # networking against host-side servers (9 checks)
make smp                 # SMP/ACPI: boot at 1/2/4/8 CPUs, TLB, power button, 2-min stress
make stress              # 30-minute -smp 8 stress (0.5 acceptance)
make posix               # 0.6 acceptance (10 scenarios incl. the Lua test suite)
python tools/aurora_cc.py prog.c -o prog   # cross-compile a program
AURORA_TEST_SMP=4 make integration   # any guest suite at 4 CPUs
make soak                # 240 s random input + idle
make dist                # build/dist/aurora-os-0.6.0.zip
```

Test artefacts (JSON results, serial logs, screenshots) go to
`build/test-results/`.

## Known gaps and next steps

1. **Desktop still in the kernel.** User processes are isolated, but the
   Terminal, Files, Notes, Settings and the compositor are kernel code.
   Moving them out needs IPC (message channels) and shared-memory surfaces.
   This is the largest remaining 0.2 roadmap item.
2. **Big kernel lock.** SMP works (1–16 CPUs), but system calls, the VFS,
   PFS, the network stack, the drivers and the desktop run under the BKL.
   Next: per-subsystem locks (VFS first), and a per-address-space lock so
   user page faults (demand paging, copy-on-write) leave the BKL. Also not
   done in 0.5: an AML interpreter and per-CPU run queues
   (`docs/decisions.md`).
3. **0.6 deviations** (`docs/decisions.md` D-0.6-*): no shared memory
   (`MAP_SHARED` fails), no job control (stop signals are ignored), no
   links, the session runs as root, the Lua test suite runs its portable
   subset (`_U`). FPU state is saved eagerly on every switch (FXSAVE).
4. **TCP** is a client without congestion control or listen/accept. **No
   TLS.**
5. **PFS** rewrites the whole catalog on every commit and keeps each file in
   one extent (64 KiB per file, 512 entries). It is fine for notes, not for
   large data.
6. **Linux reference environment not executed.** The `Dockerfile` and
   `tools/setup.sh` are written but were not run (Windows dev host without
   Docker/WSL).
7. **Interactive GUI launch** (`make run` with the GTK window) is not
   exercised by automation. It uses the same command as the tests except
   `-display` and `-serial stdio`.
8. **Real hardware**: untested and not claimed. There is no USB stack, and it
   needs PS/2 emulation and an 8259 PIC.
9. Not started: keyboard layouts, file dialogs, proportional fonts, more
   graphics modes, audio, USB HID (planned for 0.7/0.8).
10. Next: 0.7 (desktop maturity). It starts with shared-memory surfaces
   (`MAP_SHARED`/shm, deferred from 0.6) and moving apps into processes.

## Toolchain on the dev machine (installed with winget)

LLVM 23.1.5, llvm-mingw 20261020, QEMU 11.1.0, Python 3.12.10, GNU make
(ezwinports). On Windows, refresh PATH in a new shell, or call
`C:\Program Files\LLVM\bin\clang.exe` directly; `tools/aurora_env.py`
searches the standard install folders.
