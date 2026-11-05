#!/bin/sh
# Discover the Aurora OS build/test dependencies on a Linux host and print
# what is missing. Nothing is installed automatically.
set -u
missing=0
check() {
    if command -v "$1" >/dev/null 2>&1; then
        printf '  ok       %-22s %s\n' "$1" "$(command -v "$1")"
    else
        printf '  MISSING  %-22s (%s)\n' "$1" "$2"
        missing=1
    fi
}
echo "Aurora OS dependency check (host: $(uname -m), guest: x86_64)"
check python3 "Python 3.8+"
check clang "LLVM clang >= 15"
check ld.lld "LLVM lld"
check make "GNU make"
check qemu-system-x86_64 "QEMU system emulator"
check cc "host C compiler for 'make test' and the limine utility"
found_ovmf=0
for f in /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd /usr/share/edk2/x64/OVMF_CODE.4m.fd /usr/share/qemu/edk2-x86_64-code.fd; do
    [ -f "$f" ] && { echo "  ok       OVMF                   $f"; found_ovmf=1; break; }
done
[ $found_ovmf = 1 ] || { echo "  MISSING  OVMF firmware (or set AURORA_OVMF_CODE / AURORA_OVMF_VARS)"; missing=1; }
if [ $missing = 1 ]; then
    echo
    echo "Debian/Ubuntu: sudo apt install python3 clang lld make qemu-system-x86 ovmf gcc"
    echo "Fedora:        sudo dnf install python3 clang lld make qemu-system-x86 edk2-ovmf gcc"
    exit 1
fi
echo "All dependencies found. Next: make && make run"
