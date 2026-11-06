"""Shared tool discovery and QEMU command construction for Aurora OS scripts.

Host architecture and guest architecture are kept explicit: the guest is always
x86-64 (qemu-system-x86_64, `pc` machine). The host may be x86-64 or arm64; on
arm64 hosts QEMU emulates the guest (TCG). KVM/WHPX acceleration is never
required.
"""
import os
import platform
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
IS_WINDOWS = os.name == "nt"
EXE = ".exe" if IS_WINDOWS else ""

GUEST_ARCH = "x86_64"
HOST_ARCH = platform.machine().lower()

_WIN_LLVM = [Path(r"C:\Program Files\LLVM\bin")]
_WIN_QEMU = [Path(r"C:\Program Files\qemu")]


def _find(names, extra_dirs=(), env=None):
    if env and os.environ.get(env):
        p = Path(os.environ[env])
        if p.exists():
            return str(p)
        found = shutil.which(os.environ[env])
        if found:
            return found
    for n in names:
        found = shutil.which(n)
        if found:
            return found
        for d in extra_dirs:
            cand = Path(d) / (n + EXE)
            if cand.exists():
                return str(cand)
    return None


def require(tool, path):
    if not path:
        hints = {
            "clang": "install LLVM/Clang >= 15 (Windows: winget install LLVM.LLVM; Debian/Ubuntu: apt install clang lld)",
            "ld.lld": "install LLD (part of LLVM; Debian/Ubuntu: apt install lld)",
            "qemu-system-x86_64": "install QEMU (Windows: winget install SoftwareFreedomConservancy.QEMU; Debian/Ubuntu: apt install qemu-system-x86)",
            "ovmf": "install OVMF/edk2 firmware (Debian/Ubuntu: apt install ovmf) or set AURORA_OVMF_CODE/AURORA_OVMF_VARS",
            "host-cc": "install a host C compiler with a C library (Windows: winget install MartinStorsjo.LLVM-MinGW.UCRT; Linux: clang or gcc)",
        }
        sys.exit(f"error: required tool '{tool}' not found. Hint: {hints.get(tool, '')}")
    return path


def clang():
    return _find(["clang"], _WIN_LLVM, env="AURORA_CC")


def lld():
    return _find(["ld.lld"], _WIN_LLVM, env="AURORA_LD")


def host_cc():
    if IS_WINDOWS:
        return _find(["x86_64-w64-mingw32-clang", "gcc"], env="AURORA_HOST_CC")
    return _find(["clang", "cc", "gcc"], env="AURORA_HOST_CC")


def qemu():
    return _find(["qemu-system-x86_64"], _WIN_QEMU, env="AURORA_QEMU")


def ovmf():
    """Return (code_fd, vars_template_fd) or (None, None)."""
    code = os.environ.get("AURORA_OVMF_CODE")
    vars_ = os.environ.get("AURORA_OVMF_VARS")
    if code and vars_:
        return code, vars_
    qemu_bin = qemu()
    candidates = []
    if qemu_bin:
        qdir = Path(qemu_bin).resolve().parent
        candidates += [
            (qdir / "share" / "edk2-x86_64-code.fd", qdir / "share" / "edk2-i386-vars.fd"),
            (qdir.parent / "share" / "qemu" / "edk2-x86_64-code.fd", qdir.parent / "share" / "qemu" / "edk2-i386-vars.fd"),
        ]
    candidates += [
        (Path("/usr/share/OVMF/OVMF_CODE_4M.fd"), Path("/usr/share/OVMF/OVMF_VARS_4M.fd")),
        (Path("/usr/share/OVMF/OVMF_CODE.fd"), Path("/usr/share/OVMF/OVMF_VARS.fd")),
        (Path("/usr/share/edk2/x64/OVMF_CODE.4m.fd"), Path("/usr/share/edk2/x64/OVMF_VARS.4m.fd")),
        (Path("/usr/share/qemu/edk2-x86_64-code.fd"), Path("/usr/share/qemu/edk2-i386-vars.fd")),
        (Path("/opt/homebrew/share/qemu/edk2-x86_64-code.fd"), Path("/opt/homebrew/share/qemu/edk2-i386-vars.fd")),
        (Path("/usr/local/share/qemu/edk2-x86_64-code.fd"), Path("/usr/local/share/qemu/edk2-i386-vars.fd")),
    ]
    for c, v in candidates:
        if c.exists() and v.exists():
            return str(c), str(v)
    return None, None


def qemu_command(image, vars_copy, memory="256M", firmware="uefi", serial=None,
                 qmp=None, display=None, extra=None, no_reboot=True, gdb=False,
                 data_disk=None, network=None, smp=1):
    """Build the reference QEMU command line as a list of arguments."""
    cmd = [require("qemu-system-x86_64", qemu()),
           "-machine", "pc",
           "-cpu", "qemu64,+nx",
           "-smp", str(smp),
           "-m", memory,
           "-vga", "std",
           "-drive", f"file={image},format=raw,if=ide,index=0,media=disk"]
    if data_disk:
        # Persistent /home: a raw image on a legacy-capable virtio-blk device.
        cmd += ["-drive", f"file={data_disk},format=raw,if=none,id=data,cache=writeback",
                "-device", "virtio-blk-pci,drive=data"]
    if network:
        # User-mode networking (slirp): guest 10.0.2.15, gateway/host 10.0.2.2,
        # DNS 10.0.2.3. `network` may carry extra netdev options.
        opts = "" if network is True else "," + network
        cmd += ["-netdev", f"user,id=n0{opts}", "-device", "virtio-net-pci,netdev=n0"]
    else:
        cmd += ["-nic", "none"]
    if firmware == "uefi":
        code, _ = ovmf()
        require("ovmf", code)
        cmd += ["-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={code}",
                "-drive", f"if=pflash,format=raw,unit=1,file={vars_copy}"]
    if serial:
        cmd += ["-serial", serial]
    if qmp:
        cmd += ["-qmp", qmp]
    if display:
        cmd += ["-display", display]
    if no_reboot:
        cmd += ["-no-reboot"]
    if gdb:
        cmd += ["-s", "-S"]
    if extra:
        cmd += list(extra)
    return cmd
