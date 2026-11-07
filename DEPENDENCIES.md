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
| Spleen bitmap font | **2.2.0** | https://github.com/fcambus/spleen (tag `2.2.0`): `spleen-8x16.bdf`, `spleen-12x24.bdf` | SHA-256 8x16 `4a3d97ee61a8c86a7525d8c723cb8a14081f395cd2feb4227ba5e3baf0629bae`, 12x24 `181989f2c5f47b34c2474cd424b387b0002aa77fc970e59ee31e16733af24687` | BSD-2-Clause (`assets/licenses/spleen-LICENSE.txt`) | Kernel console text; since 0.7 the desktop's fallback when a TrueType font cannot be loaded. Converted to C at build time by `tools/gen_assets.py` |

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

## Since 0.1

Releases 0.2–0.4 add no third-party code to the image. The VirtIO, PCI,
ACPI, network-protocol, filesystem and user-runtime code is original. The
user programs in `/system/bin` are built from `user/` with the same clang/lld
toolchain. Tests use QEMU's `virtio-blk-pci` and `virtio-net-pci` devices and
user-mode networking (slirp), all part of the QEMU build listed above. The
host-side test servers in `tools/network_test.py` use only the Python
standard library.

## Since 0.6: user-space libraries and ports

These are compiled into the user programs in `/system/bin` (not into the
kernel). Their license texts are installed in the image under
`/system/licenses/`.

| Component | Version | Source | Integrity | License | Used for |
|---|---|---|---|---|---|
| newlib (libc + libm) | **4.6.0.20260123** | https://sourceware.org/pub/newlib/newlib-4.6.0.20260123.tar.gz | SHA-512 `ffa16d6465c0b429264c46395fa760fbcf072d3ff86e87330ba1f483efcfe66393ef83b03932759444a0ebeaef94d3ca58a59e91ab7b97b2a6ac6be2e7589657` (as published in sourceware's `sha512.sum`; checked by `tools/fetch_deps.py`) | BSD-style, per file (`COPYING.NEWLIB`, installed as `/system/licenses/newlib-COPYING.NEWLIB`) | the C library of every user program: stdio, stdlib, string, time, regex, glob, getopt, libm |
| Lua | **5.4.9** | https://www.lua.org/ftp/lua-5.4.9.tar.gz | SHA-256 `2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6` (as listed on lua.org/ftp) | MIT (`/system/licenses/lua-LICENSE.txt`) | `/system/bin/lua`, the 0.6 "ported interpreter" |
| Lua test suite | **5.4.9** | https://www.lua.org/tests/lua-5.4.9-tests.tar.gz | SHA-256 `7d971845f545ffc09fbb3128a86b2c6524161c70d0fdf0154a16e8c00c343fca` (as listed on lua.org/tests) | MIT | `/system/share/lua-tests`, run by `make posix` |

The archives are only unpacked (symbolic links and paths outside the
archive's top directory are rejected). Their build systems are not run:
`tools/userland.py` compiles newlib from the file lists in its
`Makefile.inc` fragments and Lua from `src/*.c` (without `luac.c`). The
deviations from upstream are build-time edits of installed newlib headers,
listed in `HEADER_PATCHES` / `HEADER_REPLACEMENTS` in `tools/userland.py`;
no newlib or Lua source file is modified.

Aurora's own code linked into user programs: the libc OS layer
(`user/libc/src`), the shell and utilities (`user/bin`), and
`kernel/lib/crc32.c`.

## Since 0.7: fonts and the TrueType rasterizer

Linked into (stb_truetype) or read by (DejaVu) the desktop programs; license
texts are installed under `/system/licenses/`. Neither project publishes a
digest next to its download, so each is pinned to the SHA-256 of the file
downloaded and inspected on 2026-11-06 (`tools/fetch_deps.py` refuses any
other content).

| Component | Version | Source | Integrity | License | Used for |
|---|---|---|---|---|---|
| stb_truetype.h | **1.26**, commit `6e9f34d5429cf16790ec43c9bac3f1ee4ad1f760` (last commit touching the file) | https://raw.githubusercontent.com/nothings/stb/6e9f34d5429cf16790ec43c9bac3f1ee4ad1f760/stb_truetype.h | SHA-256 `ecd30b05e0dd4fea3a13c26810dd9e1992dc379049482c393d5a19e6b5090aab` | Public domain (Unlicense), MIT alternative (`/system/licenses/stb_truetype-LICENSE.txt`) | TrueType glyph rasterization in `user/gui/common/text.c` |
| DejaVu fonts | **2.37** | https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_2_37/dejavu-fonts-ttf-2.37.tar.bz2 | SHA-256 `fa9ca4d13871dd122f61258a80d01751d603b4d3ee14095d65453b4e846e17d7` | Bitstream Vera / DejaVu changes public domain (`/system/licenses/dejavu-LICENSE.txt`) | `DejaVuSans.ttf`, `DejaVuSans-Bold.ttf`, `DejaVuSansMono.ttf` in `/system/fonts` |

The keymaps (`initramfs/system/keymaps`) and the desktop code are original.
