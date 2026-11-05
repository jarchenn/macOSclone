# Dependencies

Aurora's kernel, drivers, memory management, event system, filesystems,
compositor, desktop and applications are all original code in this
repository. Everything below is infrastructure (bootloader, font, toolchain,
emulator) as permitted by the project brief.

## Shipped in the boot image

| Component | Version | Source | Integrity | License | Used for |
|---|---|---|---|---|---|
| Limine bootloader (binary release) | **12.9.3** (released 2026-10-06) | https://github.com/limine-bootloader/limine/releases/download/v12.9.3/limine-binary.zip | SHA-256 `1bdbbcda650c5dba9b1eca5833d598137bf11bd6289bc89b6bcc4f8f112fa676` (checked by `tools/fetch_deps.py`) | BSD-2-Clause (`third_party/limine-12.9.3/LICENSE`, copied into `make dist` as `image/LIMINE-LICENSE.txt`) | `BOOTX64.EFI` (UEFI), `limine-bios.sys` + `limine bios-install` (BIOS) |
| Limine protocol header `limine.h` | limine-protocol commit `3a0526b700e356f0eac1b71a77697b3fd1c707a3` (2026-09-26, last protocol commit before 12.9.3) | https://github.com/limine-bootloader/limine-protocol | SHA-256 `0aadf2633c85d8cb145344c968add158aeb1deb68af567d55ba8431c492b6d4c` (vendored at `kernel/include/limine.h`) | 0BSD | Request/response structures, base revision 6 |
| Spleen bitmap font | **2.2.0** | https://github.com/fcambus/spleen (tag `2.2.0`): `spleen-8x16.bdf`, `spleen-12x24.bdf` | SHA-256 8x16 `4a3d97ee61a8c86a7525d8c723cb8a14081f395cd2feb4227ba5e3baf0629bae`, 12x24 `181989f2c5f47b34c2474cd424b387b0002aa77fc970e59ee31e16733af24687` | BSD-2-Clause (`assets/licenses/spleen-LICENSE.txt`) | All UI text, converted to C at build time by `tools/gen_assets.py` |

The Limine host utility (`limine bios-install`) is **compiled from the
`limine.c` source shipped in the verified release** with the host C
compiler; the prebuilt `limine.exe` in the archive is not executed.

## Toolchain and emulator (not shipped)

Versions actually used to build and test this release on the development
machine (Windows 11 x86-64 host):

| Tool | Version | Source | License |
|---|---|---|---|
| LLVM clang / ld.lld | 23.1.5 | winget `LLVM.LLVM` | Apache-2.0 with LLVM exception |
| llvm-mingw (host tests, Limine utility) | 20261020 (clang 23.1.4, UCRT) | winget `MartinStorsjo.LLVM-MinGW.UCRT` | Apache-2.0 / mingw-w64 licenses |
| QEMU | 11.1.0 | winget `SoftwareFreedomConservancy.QEMU` | GPL-2.0 |
| edk2 / OVMF firmware | bundled with QEMU 11.1.0 (`share/edk2-x86_64-code.fd`, `edk2-i386-vars.fd`) | QEMU distribution | BSD-2-Clause-Patent |
| SeaBIOS | bundled with QEMU 11.1.0 | QEMU distribution | LGPL-3.0 |
| Python | 3.12.10 | winget `Python.Python.3.12` | PSF |
| GNU make | ezwinports build | winget `ezwinports.make` | GPL-3.0 |

The Linux reference environment (`Dockerfile`, Ubuntu 24.04 packages
`clang lld make python3 qemu-system-x86 ovmf gcc`) is provided but was not
executed during this release; see `STATUS.md`.
