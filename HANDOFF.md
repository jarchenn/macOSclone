# Handoff

## State

Releases 0.1–0.4 are implemented, plus these roadmap "Later" items: window
resizing, delete and rename, a text clipboard, the Caps Lock LED and ACPI
power-off. `STATUS.md` has the evidence for each. Work since 0.1 is on branch
`roadmap-0.2-0.4`. `main` (pushed to `origin`, github.com/jarchenn/macOSclone)
holds 0.1. `build/` and `third_party/` are ignored; `make` re-downloads and
verifies Limine.

## Reproduce everything

```bash
make                     # release image -> build/aurora.img
make test                # host unit tests (9 suites)
make check               # ELF/image checks
make smoke               # boots: UEFI 128/256/512 MiB, BIOS 256 MiB, panic diagnostic
make integration         # 23 QMP-driven guest scenarios (test build)
make storage             # data-disk persistence + crash consistency (5 scenarios)
make network             # networking against host-side servers (9 checks)
make soak                # 240 s random input + idle
make dist                # build/dist/aurora-os-0.4.0.zip
```

Test artefacts (JSON results, serial logs, screenshots) go to
`build/test-results/`.

## Known gaps and next steps

1. **Desktop still in the kernel.** User processes are isolated, but the
   Terminal, Files, Notes, Settings and the compositor are kernel code.
   Moving them out needs IPC (message channels) and shared-memory surfaces.
   This is the largest remaining 0.2 roadmap item.
2. **Kernel is not preemptible.** This is fine on one CPU. SMP would need
   locking throughout, LAPIC timers and IOAPIC routing.
3. **No FPU/SSE state** is saved, so user programs are built with
   `-mgeneral-regs-only`. Next step: `fxsave`/`xsave` areas per task with
   lazy switching.
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
   graphics modes, audio, USB HID, SMP.
10. Next: the user's "OS Roadmap 0.5 and Beyond" plan, starting with 0.5
   (SMP and ACPI).

## Toolchain on the dev machine (installed with winget)

LLVM 23.1.5, llvm-mingw 20261020, QEMU 11.1.0, Python 3.12.10, GNU make
(ezwinports). On Windows, refresh PATH in a new shell, or call
`C:\Program Files\LLVM\bin\clang.exe` directly; `tools/aurora_env.py`
searches the standard install folders.
