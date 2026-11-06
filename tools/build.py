#!/usr/bin/env python3
"""Aurora OS build driver (Python 3 standard library only).

Usage: python tools/build.py <command> [options]

Commands:
  build [--variant release|test|panic]   build kernel, initramfs and disk image
  run   [--variant V] [--memory M] [--bios]   interactive QEMU
  debug [--variant V]                    QEMU paused with GDB stub on tcp::1234
  test                                   portable host tests
  check [--variant V]                    image and linkage consistency checks
  clean                                  remove build/ output
  dist                                   package source, image and docs
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import aurora_env as env  # noqa: E402
import fetch_deps  # noqa: E402
import mkfat  # noqa: E402
import make_initramfs  # noqa: E402

ROOT = env.ROOT
BUILD = env.BUILD
VERSION = "0.1.0"

VARIANTS = {
    "release": [],
    "test": ["-DAURORA_TEST=1"],
    "panic": ["-DAURORA_TEST=1", "-DAURORA_PANIC_TEST=1"],
}

KERNEL_CFLAGS = [
    "--target=x86_64-unknown-none-elf",
    "-std=c11", "-ffreestanding", "-nostdlibinc", "-fno-builtin-printf",
    "-fno-stack-protector", "-fno-stack-check", "-fno-pic", "-fno-pie",
    "-mno-red-zone", "-mcmodel=kernel", "-mgeneral-regs-only",
    "-mno-mmx", "-mno-sse", "-mno-sse2", "-mno-80387",
    "-fno-omit-frame-pointer", "-fno-strict-aliasing", "-fwrapv",
    "-O2", "-g",
    "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wvla", "-Wshadow",
    "-Werror=implicit-function-declaration", "-Werror=return-type",
    "-Werror=incompatible-pointer-types", "-Werror=int-conversion",
]

USER_CFLAGS = [
    "--target=x86_64-unknown-none-elf",
    "-std=c11", "-ffreestanding", "-nostdlibinc", "-fno-builtin-printf",
    "-fno-stack-protector", "-fno-stack-check", "-fno-pic", "-fno-pie",
    "-mgeneral-regs-only", "-mno-mmx", "-mno-sse", "-mno-sse2", "-mno-80387",
    "-fno-omit-frame-pointer", "-fno-strict-aliasing", "-fwrapv", "-O2", "-g",
    "-Wall", "-Wextra", "-Wno-unused-parameter",
]
# The user runtime reuses the kernel's freestanding string/format code.
USER_LIB_SOURCES = [
    "user/lib/crt0.S", "user/lib/libc.c",
    "kernel/lib/format.c", "kernel/lib/string.c", "kernel/lib/memory.c", "kernel/lib/crc32.c",
]

# Kernel modules that are hardware independent and compiled into host tests.
HOST_TEST_SOURCES = [
    "kernel/lib/format.c",
    "kernel/lib/string.c",
    "kernel/lib/crc32.c",
    "kernel/graphics/rect.c",
    "kernel/drivers/kbd_decode.c",
    "kernel/drivers/mouse_decode.c",
    "kernel/core/event_queue.c",
    "kernel/fs/path.c",
    "kernel/fs/vfs.c",
    "kernel/fs/ramfs.c",
    "kernel/fs/initramfs.c",
    "kernel/apps/cmdline.c",
    "kernel/apps/textbuf.c",
    "kernel/memory/heap.c",
    "kernel/memory/physical.c",
    "kernel/core/elf.c",
    "kernel/fs/pfs.c",
]


def run(cmd, **kw):
    try:
        subprocess.run(cmd, check=True, **kw)
    except FileNotFoundError:
        sys.exit(f"error: cannot execute {cmd[0]}")
    except subprocess.CalledProcessError as exc:
        sys.exit(f"error: command failed ({exc.returncode}): {' '.join(map(str, cmd))}")


def rel(p: Path) -> str:
    try:
        return str(p.relative_to(ROOT))
    except ValueError:
        return str(p)


def needs_rebuild(obj: Path, dep: Path, src: Path, extra=()) -> bool:
    if not obj.exists() or not dep.exists():
        return True
    t = obj.stat().st_mtime
    if src.stat().st_mtime > t:
        return True
    for e in extra:
        if Path(e).stat().st_mtime > t:
            return True
    text = dep.read_text(errors="replace").replace("\\\n", " ")
    _, _, deps = text.partition(":")
    for d in deps.split():
        d = d.replace("\\ ", " ")
        p = Path(d)
        if not p.is_absolute():
            p = ROOT / p
        if not p.exists() or p.stat().st_mtime > t:
            return True
    return False


def git_id() -> str:
    try:
        out = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT,
                             capture_output=True, text=True, timeout=10)
        if out.returncode == 0:
            return out.stdout.strip()
    except Exception:
        pass
    return "local"


def generate(gen: Path) -> Path:
    gen.mkdir(parents=True, exist_ok=True)
    out = gen / "assets.c"
    srcs = [ROOT / "assets/fonts/spleen-8x16.bdf", ROOT / "assets/fonts/spleen-12x24.bdf",
            ROOT / "tools/gen_assets.py"]
    if not out.exists() or any(s.stat().st_mtime > out.stat().st_mtime for s in srcs):
        run([sys.executable, str(ROOT / "tools/gen_assets.py"), str(srcs[0]), str(srcs[1]), str(out)])
    return out


def build_kernel(variant: str) -> Path:
    cc = env.require("clang", env.clang())
    ld = env.require("ld.lld", env.lld())
    vdir = BUILD / variant
    objdir = vdir / "obj"
    gen = vdir / "gen"
    asset_c = generate(gen)
    defines = [f'-DAURORA_VERSION="{VERSION}"', f'-DAURORA_BUILD_ID="{git_id()}"',
               f'-DAURORA_VARIANT="{variant}"'] + VARIANTS[variant]
    incs = ["-I" + str(ROOT / "kernel/include"), "-I" + str(ROOT / "kernel"), "-I" + str(gen)]
    sources = sorted((ROOT / "kernel").rglob("*.c")) + sorted((ROOT / "kernel").rglob("*.S"))
    sources = [s for s in sources if "host" not in s.parts] + [asset_c]
    objs = []
    stamp = vdir / "flags.txt"
    flagline = " ".join(KERNEL_CFLAGS + defines)
    flags_changed = not stamp.exists() or stamp.read_text() != flagline
    vdir.mkdir(parents=True, exist_ok=True)
    if flags_changed:
        stamp.write_text(flagline)
    for src in sources:
        name = rel(src).replace("\\", "/").replace("/", "_")
        obj = objdir / (name + ".o")
        dep = objdir / (name + ".d")
        objs.append(obj)
        if not flags_changed and not needs_rebuild(obj, dep, src):
            continue
        obj.parent.mkdir(parents=True, exist_ok=True)
        print(f"  CC  {rel(src)}")
        run([cc] + KERNEL_CFLAGS + defines + incs + ["-MMD", "-MF", str(dep), "-c", str(src), "-o", str(obj)])
    elf = vdir / "kernel.elf"
    script = ROOT / "kernel/linker.ld"
    if (not elf.exists() or any(o.stat().st_mtime > elf.stat().st_mtime for o in objs)
            or script.stat().st_mtime > elf.stat().st_mtime):
        print(f"  LD  {rel(elf)}")
        run([ld, "-nostdlib", "-static", "-z", "max-page-size=0x1000", "-z", "noexecstack",
             "--build-id=none", "--no-undefined", "-T", str(script), "-o", str(elf)] + [str(o) for o in objs])
    return elf


def compile_obj(cc, flags, src: Path, objdir: Path, force: bool) -> Path:
    name = rel(src).replace("\\", "/").replace("/", "_")
    obj = objdir / (name + ".o")
    dep = objdir / (name + ".d")
    if force or needs_rebuild(obj, dep, src):
        obj.parent.mkdir(parents=True, exist_ok=True)
        print(f"  UCC {rel(src)}")
        run([cc] + flags + ["-MMD", "-MF", str(dep), "-c", str(src), "-o", str(obj)])
    return obj


def build_user(variant: str) -> dict:
    """Builds every user/bin/*.c program; returns {name: elf_path}."""
    cc = env.require("clang", env.clang())
    ld = env.require("ld.lld", env.lld())
    udir = BUILD / variant / "user"
    objdir = udir / "obj"
    flags = USER_CFLAGS + ["-I" + str(ROOT / "user/include"), "-I" + str(ROOT / "kernel/include"),
                           f'-DAURORA_VERSION="{VERSION}"']
    stamp = udir / "flags.txt"
    flagline = " ".join(flags)
    force = not stamp.exists() or stamp.read_text() != flagline
    udir.mkdir(parents=True, exist_ok=True)
    stamp.write_text(flagline)
    lib_objs = [compile_obj(cc, flags, ROOT / s, objdir, force) for s in USER_LIB_SOURCES]
    script = ROOT / "user/linker.ld"
    out = {}
    for src in sorted((ROOT / "user/bin").glob("*.c")):
        obj = compile_obj(cc, flags, src, objdir, force)
        elf = udir / src.stem
        objs = lib_objs + [obj]
        if (not elf.exists() or any(o.stat().st_mtime > elf.stat().st_mtime for o in objs)
                or script.stat().st_mtime > elf.stat().st_mtime):
            print(f"  ULD {rel(elf)}")
            run([ld, "-nostdlib", "-static", "-z", "max-page-size=0x1000", "-z", "noexecstack",
                 "--build-id=none", "--no-undefined", "-T", str(script), "-o", str(elf)] + [str(o) for o in objs])
        out[src.stem] = elf
    return out


def build_initramfs(variant: str, programs: dict) -> Path:
    out = BUILD / variant / "initramfs.img"
    extra = {f"system/bin/{name}": path.read_bytes() for name, path in programs.items()}
    data = make_initramfs.pack(ROOT / "initramfs", extra)
    if not out.exists() or out.read_bytes() != data:
        print(f"  FS  {rel(out)}")
        out.write_bytes(data)
    return out


def limine_tool() -> Path:
    """Build the Limine host utility from the release's own limine.c source."""
    tool = BUILD / "host-tools" / ("limine" + env.EXE)
    src = fetch_deps.LIMINE_DIR / "limine.c"
    if tool.exists() and tool.stat().st_mtime >= src.stat().st_mtime:
        return tool
    cc = env.host_cc()
    if not cc:
        return None
    tool.parent.mkdir(parents=True, exist_ok=True)
    print(f"  HOSTCC {rel(tool)}")
    r = subprocess.run([cc, "-O2", "-std=c99", "-I", str(fetch_deps.LIMINE_DIR), str(src), "-o", str(tool)])
    return tool if r.returncode == 0 else None


def build_image(variant: str, elf: Path, initrd: Path) -> Path:
    fetch_deps.fetch_limine()
    lim = fetch_deps.LIMINE_DIR
    conf = (ROOT / "boot/limine.conf").read_bytes()
    files = {
        "EFI/BOOT/BOOTX64.EFI": (lim / "BOOTX64.EFI").read_bytes(),
        "boot/limine/limine-bios.sys": (lim / "limine-bios.sys").read_bytes(),
        "boot/limine/limine.conf": conf,
        "boot/aurora/kernel.elf": elf.read_bytes(),
        "boot/aurora/initramfs.img": initrd.read_bytes(),
    }
    img = BUILD / variant / "aurora.img"
    print(f"  IMG {rel(img)}")
    mkfat.build_image(files, img)
    tool = limine_tool()
    if tool:
        r = subprocess.run([str(tool), "bios-install", str(img)], capture_output=True, text=True)
        if r.returncode != 0:
            print(f"warning: limine bios-install failed; image is UEFI-only\n{r.stdout}{r.stderr}")
            (BUILD / variant / "bios.txt").write_text("no")
        else:
            (BUILD / variant / "bios.txt").write_text("yes")
    else:
        print("warning: no host C compiler for the limine utility; image is UEFI-only")
        (BUILD / variant / "bios.txt").write_text("no")
    return img


def cmd_build(args):
    for v in (args.variant if isinstance(args.variant, list) else [args.variant]):
        print(f"[{v}]")
        elf = build_kernel(v)
        programs = build_user(v)
        initrd = build_initramfs(v, programs)
        img = build_image(v, elf, initrd)
        if v == "release":
            shutil.copyfile(img, BUILD / "aurora.img")
        print(f"built {rel(img)}")


def vars_copy(dirpath: Path) -> Path:
    _, vars_tmpl = env.ovmf()
    env.require("ovmf", vars_tmpl)
    dst = dirpath / "ovmf-vars.fd"
    shutil.copyfile(vars_tmpl, dst)
    return dst


def cmd_test(args):
    cc = env.require("host-cc", env.host_cc())
    out = BUILD / "host-tests"
    out.mkdir(parents=True, exist_ok=True)
    tests = sorted((ROOT / "tests/host").glob("test_*.c"))
    common = [str(ROOT / s) for s in HOST_TEST_SOURCES]
    flags = ["-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Wno-unused-parameter", "-DAURORA_HOST=1",
             '-DAURORA_VERSION="host"', '-DAURORA_BUILD_ID="host"', '-DAURORA_VARIANT="host"',
             "-I" + str(ROOT / "kernel/include"), "-I" + str(ROOT / "kernel"), "-I" + str(ROOT / "tests/host")]
    san = ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-fno-omit-frame-pointer"]
    probe = out / "probe.c"
    probe.write_text("int main(void){return 0;}\n")
    use_san = subprocess.run([cc] + san + [str(probe), "-o", str(out / ("probe" + env.EXE))],
                             capture_output=True).returncode == 0
    print(f"host compiler: {cc}  sanitizers: {'address,undefined' if use_san else 'unavailable'}")
    failed = 0
    for t in tests:
        exe = out / (t.stem + env.EXE)
        cmd = [cc] + flags + (san if use_san else []) + [str(t), str(ROOT / "tests/host/harness.c")] + common + ["-o", str(exe)]
        r = subprocess.run(cmd)
        if r.returncode != 0:
            print(f"FAIL (compile) {t.stem}")
            failed += 1
            continue
        r = subprocess.run([str(exe)])
        if r.returncode != 0:
            print(f"FAIL {t.stem} (exit {r.returncode})")
            failed += 1
    print(f"host tests: {len(tests) - failed}/{len(tests)} passed")
    sys.exit(1 if failed else 0)


def parse_elf(path: Path):
    data = path.read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        raise ValueError("not a little-endian ELF64 file")
    (e_type, e_machine, _, e_entry, e_phoff, e_shoff, _, _, e_phentsize, e_phnum,
     e_shentsize, e_shnum, e_shstrndx) = struct.unpack_from("<HHIQQQIHHHHHH", data, 16)
    phdrs = []
    for i in range(e_phnum):
        p_type, p_flags, p_off, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = \
            struct.unpack_from("<IIQQQQQQ", data, e_phoff + i * e_phentsize)
        phdrs.append(dict(type=p_type, flags=p_flags, vaddr=p_vaddr, filesz=p_filesz, memsz=p_memsz, align=p_align))
    shdrs = []
    for i in range(e_shnum):
        shdrs.append(struct.unpack_from("<IIQQQQIIQQ", data, e_shoff + i * e_shentsize))
    strtab = shdrs[e_shstrndx]
    names = []
    for sh in shdrs:
        off = strtab[4] + sh[0]
        end = data.index(b"\0", off)
        names.append(data[off:end].decode())
    return dict(type=e_type, machine=e_machine, entry=e_entry, phdrs=phdrs, sections=names, data=data, shdrs=shdrs)


def cmd_check(args):
    variants = [args.variant] if args.variant else ["release"]
    ok = True

    def check(cond, msg):
        nonlocal ok
        print(("  ok    " if cond else "  FAIL  ") + msg)
        ok &= bool(cond)

    for v in variants:
        elf_path = BUILD / v / "kernel.elf"
        img_path = BUILD / v / "aurora.img"
        if not elf_path.exists():
            sys.exit(f"error: {rel(elf_path)} missing; run 'make' first")
        e = parse_elf(elf_path)
        print(f"[{v}] {rel(elf_path)}")
        check(e["type"] == 2, "ELF type is EXEC (non-PIE)")
        check(e["machine"] == 62, "ELF machine is x86-64")
        check(e["entry"] >= 0xFFFFFFFF80000000, f"entry point in higher half (0x{e['entry']:x})")
        types = [p["type"] for p in e["phdrs"]]
        check(3 not in types, "no PT_INTERP")
        check(2 not in types, "no PT_DYNAMIC")
        for p in e["phdrs"]:
            if p["type"] == 1:
                w, x = bool(p["flags"] & 2), bool(p["flags"] & 1)
                check(not (w and x), f"PT_LOAD at 0x{p['vaddr']:x} is not both writable and executable")
                check(p["vaddr"] % 4096 == 0, f"PT_LOAD at 0x{p['vaddr']:x} is page aligned")
        check(".limine_requests" in e["sections"], "has .limine_requests section")
        for bad in (".interp", ".dynamic", ".got.plt", ".plt"):
            check(bad not in e["sections"], f"no {bad} section")
        data = e["data"]
        for magic, what in ((struct.pack("<QQ", 0xf9562b2d5c95a6c8, 0x6a7b384944536bdc), "base revision tag"),
                            (struct.pack("<QQQQ", 0xf6b8f4b39de7d1ae, 0xfab91a6940fcb9cf, 0x785c6ed015d3e316, 0x181e920a7852b9d9), "requests start marker"),
                            (struct.pack("<QQ", 0xadc0e0531bb10d03, 0x9572709f31764c62), "requests end marker")):
            check(magic in data, f"contains Limine {what}")
        # Undefined symbols: lld was run with --no-undefined; check symtab anyway.
        undefined = 0
        for idx, sh in enumerate(e["shdrs"]):
            if sh[1] == 2:  # SHT_SYMTAB
                off, size, entsize = sh[4], sh[5], sh[9]
                for i in range(1, size // entsize):
                    st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from("<IBBHQQ", data, off + i * entsize)
                    if st_shndx == 0 and (st_info >> 4) != 0:
                        undefined += 1
        check(undefined == 0, "no undefined global symbols")
        if img_path.exists():
            img = img_path.read_bytes()
            check(img[510:512] == b"\x55\xaa", "disk image has MBR signature")
            check(img[446 + 4] == 0xEF, "partition 1 type is EFI System (0xEF)")
            start = struct.unpack_from("<I", img, 446 + 8)[0]
            check(img[start * 512 + 54:start * 512 + 62] == b"FAT16   ", "partition contains FAT16")
            check(elf_path.read_bytes() in img, "image contains the current kernel.elf")
        else:
            check(False, f"{rel(img_path)} exists")
    sys.exit(0 if ok else 1)


def cmd_clean(args):
    if BUILD.exists():
        shutil.rmtree(BUILD)
    print("removed build/")


def cmd_dist(args):
    args.variant = ["release"]
    cmd_build(args)
    dist = BUILD / "dist"
    dist.mkdir(parents=True, exist_ok=True)
    out = dist / f"aurora-os-{VERSION}.zip"
    skip_dirs = {"build", "third_party", ".git", "__pycache__", ".claude"}
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as zf:
        for p in sorted(ROOT.rglob("*")):
            r = p.relative_to(ROOT)
            if any(part in skip_dirs for part in r.parts) or not p.is_file():
                continue
            zf.write(p, f"aurora-os-{VERSION}/{r.as_posix()}")
        zf.write(BUILD / "aurora.img", f"aurora-os-{VERSION}/image/aurora.img")
        zf.write(BUILD / "release" / "kernel.elf", f"aurora-os-{VERSION}/image/kernel.elf")
        lic = fetch_deps.LIMINE_DIR / "LICENSE"
        zf.write(lic, f"aurora-os-{VERSION}/image/LIMINE-LICENSE.txt")
        for shot in sorted((ROOT / "docs" / "screenshots").glob("*.png")) if (ROOT / "docs" / "screenshots").exists() else []:
            pass  # already included via the source walk
    print(f"wrote {rel(out)} ({out.stat().st_size // 1024} KiB)")


HELP = """Aurora OS 0.1 - make targets
  make              build the bootable release image (build/aurora.img)
  make run          interactive QEMU (UEFI/OVMF); MEM=512M changes guest RAM
  make run-bios     interactive QEMU with SeaBIOS
  make debug        QEMU paused with a GDB stub on tcp::1234
  make test         portable host tests (ASan/UBSan when the host compiler has them)
  make smoke        real-guest boot tests: 128/256/512 MiB UEFI, BIOS, panic diagnostic
  make integration  real-guest input / window / file tests via QMP (test build)
  make soak         4-minute random input + idle soak test
  make check        image and linkage consistency checks
  make clean        remove build/ only
  make dist         package source, image and documentation (build/dist/)

Requires: Python 3.8+, clang + ld.lld (LLVM >= 15), GNU make, qemu-system-x86_64,
OVMF/edk2 firmware (bundled with QEMU on Windows), a host C compiler with libc for
'make test', and network access once to fetch the pinned Limine release.
Without make: python tools/build.py <build|run|debug|test|check|clean|dist>."""


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "help":
        print(HELP)
        return
    ap = argparse.ArgumentParser(description="Aurora OS build driver")
    sub = ap.add_subparsers(dest="cmd")
    b = sub.add_parser("build")
    b.add_argument("--variant", nargs="+", default=["release"], choices=list(VARIANTS))
    for name in ("run", "debug"):
        r = sub.add_parser(name)
        r.add_argument("--variant", default="release", choices=list(VARIANTS))
        r.add_argument("--memory", default="256M")
        r.add_argument("--bios", action="store_true", help="boot via SeaBIOS instead of OVMF")
    sub.add_parser("test")
    c = sub.add_parser("check")
    c.add_argument("--variant", choices=list(VARIANTS))
    sub.add_parser("clean")
    sub.add_parser("dist")
    args = ap.parse_args()
    if args.cmd in (None, "build"):
        if args.cmd is None:
            args.variant = ["release"]
        cmd_build(args)
    elif args.cmd == "run":
        cmd_run_wrapper(args, gdb=False)
    elif args.cmd == "debug":
        cmd_run_wrapper(args, gdb=True)
    elif args.cmd == "test":
        cmd_test(args)
    elif args.cmd == "check":
        cmd_check(args)
    elif args.cmd == "clean":
        cmd_clean(args)
    elif args.cmd == "dist":
        cmd_dist(args)


def cmd_run_wrapper(args, gdb):
    variant = args.variant
    bargs = argparse.Namespace(variant=[variant])
    cmd_build(bargs)
    img = BUILD / variant / "aurora.img"
    rundir = BUILD / "run"
    rundir.mkdir(parents=True, exist_ok=True)
    vc = vars_copy(rundir) if not args.bios else None
    cmd = env.qemu_command(img, vc, memory=args.memory, firmware="bios" if args.bios else "uefi",
                           serial="stdio", no_reboot=False, gdb=gdb)
    print("+ " + " ".join(f'"{c}"' if " " in c else c for c in cmd))
    if gdb:
        print("GDB stub listening on tcp::1234; the guest is paused until a debugger continues it.")
        print(f"  gdb {rel(BUILD / variant / 'kernel.elf')} -ex 'target remote localhost:1234'")
    sys.exit(subprocess.call(cmd))


if __name__ == "__main__":
    main()
