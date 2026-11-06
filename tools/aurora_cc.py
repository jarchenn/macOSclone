#!/usr/bin/env python3
"""Cross-compile C programs for Aurora (0.6).

    python tools/aurora_cc.py hello.c [more.c ...] -o hello [-- extra clang flags]

Builds the sysroot first if needed (newlib + Aurora's libc layer, see
tools/userland.py), then compiles with clang for x86_64-unknown-none-elf
against build/sysroot/include and links statically with crt0, libaurora,
libc and libm using user/linker.ld. The result runs on Aurora: copy it into
user/bin's output (put the source in user/bin/ and run `make`), or onto the
data disk.
"""
import argparse
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import aurora_env as env  # noqa: E402
import userland  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("sources", nargs="+")
    ap.add_argument("-o", "--output", required=True)
    a, unknown = ap.parse_known_args()
    a.extra = unknown  # anything else goes to clang (-O3, -DNAME, -Iinclude ...)
    userland.ensure_sysroot()
    cc = env.require("clang", env.clang())
    out = Path(a.output)
    objs = []
    extra = [x for x in a.extra if x != "--"]
    for src in a.sources:
        obj = out.parent / (Path(src).stem + ".o")
        cmd = [cc] + userland.target_flags(userland.USER_PROGRAM_FLAGS + extra) + ["-c", src, "-o", str(obj)]
        if subprocess.run(cmd).returncode:
            return 1
        objs.append(obj)
    userland.link_program(objs, out, env.ROOT / "user" / "linker.ld")
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
