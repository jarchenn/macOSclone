# Handoff

## State

Release 0.1 is implemented, and every acceptance criterion in `STATUS.md`
has emulator evidence. The repository is not under version control yet (no
git history was created). To keep history, run `git init` and make an initial
commit.

## Reproduce everything

```bash
make                     # release image -> build/aurora.img
make test                # host unit tests
make check               # ELF/image checks
make smoke               # boots: UEFI 128/256/512 MiB, BIOS 256 MiB, panic diagnostic
make integration         # 13 QMP-driven guest scenarios (test build)
make soak                # 240 s random input + idle
make dist                # build/dist/aurora-os-0.1.0.zip
```

Test artefacts (JSON results, serial logs, screenshots) go to
`build/test-results/`. `docs/screenshots/` holds copies from the final run.

## Known gaps and next steps

1. **Linux reference environment not executed.** The `Dockerfile` and
   `tools/setup.sh` are written but were not run: the development host is
   Windows without Docker or WSL. Next step: `docker build -t aurora-dev .`,
   then `make test smoke integration` inside the container. Expect possible
   OVMF path differences (`AURORA_OVMF_CODE` / `AURORA_OVMF_VARS` override
   them).
2. **Interactive GUI launch not exercised by automation.** `make run` opens
   QEMU's GTK window, which is the same command as the tests except
   `-display` and `-serial stdio`. Tests use `-display none` and QMP. Manual
   check: run `make run`, click the dock, press Ctrl+Alt+G to release the grab.
3. **Real hardware**: untested, so none is claimed as supported. Likely
   blockers: USB-only keyboards (no USB stack), firmware without legacy PS/2
   emulation, a missing 8259 PIC.
4. Small UI limitations: Notes wraps by character rather than word; no window
   resizing; no delete or rename; Caps Lock LED is not updated.
5. Roadmap order: process isolation → VirtIO persistent storage → networking
   (`docs/roadmap.md`).

## Toolchain on the dev machine (installed with winget)

LLVM 23.1.5, llvm-mingw 20261020, QEMU 11.1.0, Python 3.12.10, GNU make
(ezwinports). On Windows, refresh PATH in a new shell, or call
`C:\Program Files\LLVM\bin\clang.exe` directly; `tools/aurora_env.py`
searches the standard install folders.
