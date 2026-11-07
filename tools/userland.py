"""User-space toolchain for Aurora 0.6: sysroot, newlib, Aurora's libc layer
and the programs in user/bin.

newlib (pinned, verified by tools/fetch_deps.py) is compiled from its
sources directly: the file lists come from newlib's own Makefile.inc
fragments, evaluated for Aurora's configuration (full ELIX level, no nano
formatted I/O, no syscall stubs - Aurora's libc layer provides the OS
interface). newlib's configure script and makefiles are never run.

Layout (all under build/):
  sysroot/include   newlib headers + user/libc/include overrides + aurora_abi.h
  sysroot/lib       crt0.o, libc.a (newlib), libm.a, libaurora.a
"""
import hashlib
import os
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import aurora_env as env
import fetch_deps

ROOT = env.ROOT
BUILD = env.BUILD
SYSROOT = BUILD / "sysroot"
LIBC_DIR = ROOT / "user" / "libc"

# Conditionals in newlib's Makefile.inc files and their value for Aurora.
TRUE_CONDS = {
    "HAVE_STDIO_DIR", "HAVE_POSIX_DIR", "HAVE_SIGNAL_DIR", "HAVE_UNIX_DIR",
    "HAVE_LONG_DOUBLE", "NEWLIB_RETARGETABLE_LOCKING", "HAVE_LIBC_MACHINE_X86_64",
}

LIBC_DIRS = ["argz", "stdlib", "ctype", "search", "stdio", "string", "signal", "time",
             "locale", "reent", "errno", "misc", "unix", "posix", "machine/x86_64"]
LIBM_DIRS = ["math", "common", "complex"]

# newlib files replaced by Aurora's libc layer (user/libc/src).
EXCLUDE = {
    "libc/misc/lock.c",          # retargetable locks: src/locks.c
    "libc/signal/signal.c",      # signal(): src/signal.c (on sigaction)
    "libc/signal/raise.c",       # raise(): kill(getpid())
    "libc/stdlib/system.c",      # system(): posix_spawn of /system/bin/sh
    "libc/posix/posix_spawn.c",  # native SYS_SPAWN: src/spawn.c
    "libc/posix/_isatty.c",      # isatty via TCGETS, not "is a character device"
    "libc/unix/getcwd.c",        # SYS_GETCWD
    "libc/unix/ttyname.c",
    "libc/unix/ttyname_r.c",
    "libc/unix/getpwent.c",      # fixed user table: src/misc.c
    "libc/unix/getlogin.c",
    "libc/reent/getreent.c",     # unused with _REENT_THREAD_LOCAL
    "libc/unix/getut.c",         # no utmp
    "libc/unix/getpass.c",       # BSD sigblock interface
}

# Extra compiler flags for single newlib files.
FILE_FLAGS = {
    "libc/posix/fnmatch.c": ["-D_GNU_SOURCE"],  # FNM_CASEFOLD / FNM_LEADING_DIR
}

# Small, deterministic edits to installed newlib headers (anchor, text to
# insert after it). Each anchor must exist exactly once.
HEADER_PATCHES = [
    ("sys/features.h", "#include <_newlib_version.h>\n", """
/* Aurora (0.6): POSIX options implemented by Aurora's libc layer. */
#ifdef __aurora__
#define _POSIX_THREADS 1
#define _POSIX_TIMEOUTS 1
#define _POSIX_TIMERS 1
#define _POSIX_MONOTONIC_CLOCK 200112L
#define _POSIX_CPUTIME 1
#define _POSIX_THREAD_CPUTIME 1
#define _POSIX_SPAWN 1
#define _POSIX_READER_WRITER_LOCKS 200112L
#define _POSIX_SPIN_LOCKS 200112L
#define _POSIX_BARRIERS 200112L
#define _POSIX_THREAD_SAFE_FUNCTIONS 1
#define _POSIX_THREAD_ATTR_STACKSIZE 1
#define _UNIX98_THREAD_MUTEX_ATTRIBUTES 1
/* POSIX types for read()/write() (newlib's legacy default is int). */
#define _READ_WRITE_RETURN_TYPE _ssize_t
#define _READ_WRITE_BUFSIZE_TYPE __size_t
#endif
"""),
    ("sys/stat.h", "int\tmknod (const char *__path, mode_t __mode, dev_t __dev );\n#endif\n", """#ifdef __aurora__
int lstat (const char *__restrict __path, struct stat *__restrict __buf );
#endif
"""),
    ("setjmp.h", "int\tsetjmp (jmp_buf __jmpb) __returns_twice;\n", """#ifdef __aurora__
/* BSD names without signal-mask handling (src/setjmp_alias.S). */
void _longjmp (jmp_buf __jmpb, int __retval) __dead2;
int _setjmp (jmp_buf __jmpb) __returns_twice;
#endif
"""),
    ("regex.h","#define\t_REGEX_H_\n", "\n#include <sys/types.h> /* off_t (Aurora) */\n"),
    ("stdio.h", "/* getline - see __getline for now */\n", """#ifdef __aurora__
ssize_t getdelim (char **, size_t *, int, FILE *);
ssize_t getline (char **, size_t *, FILE *);
#endif
"""),
    ("sys/signal.h", "#define SA_NOCLDSTOP 1  /* only value supported now for sa_flags */\n", """#ifdef __aurora__
#define SA_SIGINFO 0x2
#define SA_ONSTACK 0x4
#define SA_RESTART 0x10000000
#define SA_NODEFER 0x40000000
#define SA_RESETHAND 0x80000000
#define SA_NOMASK SA_NODEFER
#define SA_ONESHOT SA_RESETHAND
#endif
"""),
]


# Replacements in installed headers (old text, new text; old must occur
# exactly once).
HEADER_REPLACEMENTS = [
    # newlib's generic sigaddset() & co. macros use bit `sig` (and overflow
    # an int at signal 31), unlike its own sigset.c functions and Aurora's
    # kernel, which use bit `sig - 1`: use the functions.
    ("sys/signal.h", "#if !defined(__CYGWIN__) && !defined(__rtems__)\n/* These depend upon the type of sigset_t",
     "#if !defined(__CYGWIN__) && !defined(__rtems__) && !defined(__aurora__)\n/* These depend upon the type of sigset_t"),
]


def target_flags(cc_extra=()):
    return ["--target=x86_64-unknown-none-elf", "-D__aurora__", "-fno-pic", "-fno-pie",
            "-fno-stack-protector", "-fno-stack-check", "-ffunction-sections", "-fdata-sections",
            "-fno-omit-frame-pointer", "-nostdlibinc", "-isystem", str(SYSROOT / "include"),
            "-O2", "-g"] + list(cc_extra)


NEWLIB_DEFINES = ["-D_COMPILING_NEWLIB", "-D_LIBC", "-DHAVE_NANOSLEEP", "-DHAVE_FCNTL",
                  "-DHAVE_RENAME", "-D_NO_GETCWD", "-DSIGNAL_PROVIDED", "-fno-builtin", "-w"]


def makefile_sources(directory: Path):
    """Source files listed in newlib's Makefile.inc for `directory`."""
    mk = directory / "Makefile.inc"
    if not mk.exists():
        return []
    stack = []
    out = []
    for raw in mk.read_text(errors="replace").splitlines():
        line = raw.strip()
        if line.startswith("if "):
            cond = line[3:].strip()
            neg = cond.startswith("!")
            name = cond.lstrip("!")
            val = name in TRUE_CONDS
            stack.append(val != neg)
            continue
        if line.startswith("else"):
            if stack:
                stack[-1] = not stack[-1]
            continue
        if line.startswith("endif"):
            if stack:
                stack.pop()
            continue
        if not all(stack):
            continue
        for tok in re.findall(r"%D%/([A-Za-z0-9_\-./]+\.(?:c|S))", line):
            out.append(directory / tok)
    seen = set()
    uniq = []
    for p in out:
        if p not in seen:
            seen.add(p)
            uniq.append(p)
    return uniq


def install_headers(newlib: Path):
    inc = SYSROOT / "include"
    if inc.exists():
        shutil.rmtree(inc)
    shutil.copytree(newlib / "newlib" / "libc" / "include", inc)
    for src in (LIBC_DIR / "include").rglob("*"):
        if src.is_file():
            dst = inc / src.relative_to(LIBC_DIR / "include")
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, dst)
    shutil.copyfile(ROOT / "kernel" / "include" / "abi" / "aurora_abi.h", inc / "aurora_abi.h")
    for rel_path, anchor, text in HEADER_PATCHES:
        p = inc / rel_path
        s = p.read_text()
        if s.count(anchor) != 1:
            sys.exit(f"error: newlib header patch anchor not found exactly once in {rel_path}")
        p.write_text(s.replace(anchor, anchor + text))
    for rel_path, old, new in HEADER_REPLACEMENTS:
        p = inc / rel_path
        s = p.read_text()
        if s.count(old) != 1:
            sys.exit(f"error: newlib header replacement not found exactly once in {rel_path}")
        p.write_text(s.replace(old, new))


def tree_digest(paths) -> str:
    h = hashlib.sha256()
    for base in paths:
        base = Path(base)
        files = [base] if base.is_file() else sorted(p for p in base.rglob("*") if p.is_file())
        for f in files:
            h.update(str(f.relative_to(ROOT)).encode())
            h.update(f.read_bytes())
    return h.hexdigest()


def compile_many(cc, jobs, label):
    """jobs: list of (src, obj, flags). Compiles in parallel; stops on error."""
    def one(job):
        src, obj, flags = job
        obj.parent.mkdir(parents=True, exist_ok=True)
        r = subprocess.run([cc] + flags + ["-c", str(src), "-o", str(obj)], capture_output=True, text=True)
        return src, r
    failed = []
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        for src, r in ex.map(one, jobs):
            if r.returncode != 0:
                failed.append((src, r.stderr))
    if failed:
        for src, err in failed[:10]:
            print(f"error: {label}: {src}\n{err[:3000]}")
        sys.exit(f"error: {len(failed)} {label} source(s) failed to compile")


def archive(objs, out: Path):
    ar = env.llvm_tool("llvm-ar")
    if out.exists():
        out.unlink()
    # Response file: the object list is long on Windows command lines.
    rsp = out.with_suffix(".rsp")
    rsp.write_text("\n".join(str(o).replace("\\", "/") for o in objs))
    subprocess.run([ar, "rcs", str(out), "@" + str(rsp)], check=True)


def build_newlib():
    """Builds the sysroot (headers, libc.a, libm.a). Cached by content."""
    newlib = fetch_deps.source_dir("newlib")
    if not (newlib / ".verified").exists():
        sys.exit("error: newlib sources missing; run `make deps` (tools/fetch_deps.py)")
    cc = env.require("clang", env.clang())
    lib = SYSROOT / "lib"
    stamp = lib / "newlib.stamp"
    digest = hashlib.sha256((tree_digest([LIBC_DIR / "include", ROOT / "kernel/include/abi"]) +
                             (newlib / ".verified").read_text() + " ".join(target_flags(NEWLIB_DEFINES)) +
                             repr(sorted(EXCLUDE)) + repr(HEADER_PATCHES) + repr(HEADER_REPLACEMENTS) + repr(FILE_FLAGS) + repr(sorted(TRUE_CONDS)))
                            .encode()).hexdigest()
    if stamp.exists() and stamp.read_text() == digest and (lib / "libc.a").exists():
        return
    print(f"  NEWLIB {newlib.name}: headers")
    install_headers(newlib)
    objdir = BUILD / "newlib-obj"
    if objdir.exists():
        shutil.rmtree(objdir)
    base = newlib / "newlib"
    jobs_c, jobs_m = [], []
    for d in LIBC_DIRS:
        for src in makefile_sources(base / "libc" / d):
            rel_s = src.relative_to(base).as_posix()
            if rel_s in EXCLUDE:
                continue
            obj = objdir / (rel_s.replace("/", "_") + ".o")
            jobs_c.append((src, obj, target_flags(NEWLIB_DEFINES) + ["-I", str(src.parent)] + FILE_FLAGS.get(rel_s, [])))
    for d in LIBM_DIRS:
        for src in makefile_sources(base / "libm" / d):
            rel_s = src.relative_to(base).as_posix()
            obj = objdir / (rel_s.replace("/", "_") + ".o")
            jobs_m.append((src, obj, target_flags(NEWLIB_DEFINES) +
                           ["-I", str(src.parent), "-I", str(base / "libm" / "common")]))
    print(f"  NEWLIB compiling {len(jobs_c)} libc and {len(jobs_m)} libm sources")
    compile_many(cc, jobs_c + jobs_m, "newlib")
    lib.mkdir(parents=True, exist_ok=True)
    archive([j[1] for j in jobs_c], lib / "libc.a")
    archive([j[1] for j in jobs_m], lib / "libm.a")
    stamp.write_text(digest)


def build_libaurora():
    """Aurora's libc layer: crt0.o and libaurora.a."""
    cc = env.require("clang", env.clang())
    lib = SYSROOT / "lib"
    srcs = sorted((LIBC_DIR / "src").glob("*.c")) + sorted((LIBC_DIR / "src").glob("*.S"))
    srcs = [s for s in srcs if s.name != "crt0.S"] + [ROOT / "kernel" / "lib" / "crc32.c"]
    flags = target_flags(["-std=gnu11", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Werror=implicit-function-declaration",
                          "-I", str(LIBC_DIR / "src")])
    digest = hashlib.sha256((tree_digest([LIBC_DIR, ROOT / "kernel/lib/crc32.c", ROOT / "kernel/include/abi"]) +
                             " ".join(flags) + (lib / "newlib.stamp").read_text()).encode()).hexdigest()
    stamp = lib / "aurora.stamp"
    if stamp.exists() and stamp.read_text() == digest and (lib / "libaurora.a").exists():
        return
    objdir = BUILD / "libaurora-obj"
    if objdir.exists():
        shutil.rmtree(objdir)
    jobs = [(s, objdir / (s.name + ".o"), flags + (["-I", str(ROOT / "kernel/include")] if s.name == "crc32.c" else []))
            for s in srcs]
    jobs.append((LIBC_DIR / "src" / "crt0.S", lib / "crt0.o", flags))
    print(f"  LIBC   Aurora libc layer ({len(jobs)} sources)")
    compile_many(cc, jobs, "libaurora")
    archive([j[1] for j in jobs if j[1].name != "crt0.o"], lib / "libaurora.a")
    stamp.write_text(digest)


def ensure_sysroot():
    build_newlib()
    build_libaurora()


USER_PROGRAM_FLAGS = ["-std=gnu11", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Werror=implicit-function-declaration",
                      "-Werror=return-type", "-Werror=incompatible-pointer-types", "-Werror=int-conversion"]


def link_program(objs, out: Path, script: Path):
    ld = env.require("ld.lld", env.lld())
    lib = SYSROOT / "lib"
    cmd = [ld, "-nostdlib", "-static", "-z", "max-page-size=0x1000", "-z", "noexecstack", "--build-id=none",
           "--gc-sections", "--no-undefined", "--strip-debug", "-T", str(script), "-o", str(out),
           str(lib / "crt0.o")] + [str(o) for o in objs] + [
           "--start-group", str(lib / "libaurora.a"), str(lib / "libc.a"), str(lib / "libm.a"), "--end-group"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout + r.stderr)
        sys.exit(f"error: link failed for {out.name}")


def build_ports(variant: str) -> dict:
    """Third-party programs built against the sysroot: Lua 5.4 (0.6).
    Returns {name: elf}."""
    lua = fetch_deps.source_dir("lua")
    if not (lua / ".verified").exists():
        sys.exit("error: Lua sources missing; run `make deps` (tools/fetch_deps.py)")
    cc = env.require("clang", env.clang())
    objdir = BUILD / variant / "ports" / "lua"
    flags = target_flags(["-std=gnu99", "-DLUA_USE_POSIX", "-w"])
    stamp = objdir / "flags.txt"
    line = " ".join(flags) + (SYSROOT / "lib" / "aurora.stamp").read_text()
    force = not stamp.exists() or stamp.read_text() != line
    srcs = [f for f in sorted((lua / "src").glob("*.c")) if f.name != "luac.c"]
    jobs = []
    objs = []
    for src in srcs:
        obj = objdir / (src.stem + ".o")
        objs.append(obj)
        if force or not obj.exists() or obj.stat().st_mtime < src.stat().st_mtime:
            jobs.append((src, obj, flags))
    if jobs:
        print(f"  LUA    compiling {len(jobs)} sources ({lua.name})")
        compile_many(cc, jobs, "lua")
    objdir.mkdir(parents=True, exist_ok=True)
    stamp.write_text(line)
    elf = BUILD / variant / "user" / "lua"
    script = ROOT / "user" / "linker.ld"
    libs = [SYSROOT / "lib" / n for n in ("crt0.o", "libaurora.a", "libc.a", "libm.a")]
    if not elf.exists() or any(d.stat().st_mtime > elf.stat().st_mtime for d in objs + libs + [script]):
        print("  ULD    lua")
        link_program(objs, elf, script)
    return {"lua": elf}


def port_data(with_tests=True) -> dict:
    """Read-only data shipped in /system: the licenses of the code linked
    into user programs, and Lua's test suite (0.6)."""
    tests = fetch_deps.source_dir("lua-tests")
    out = {}
    newlib = fetch_deps.source_dir("newlib")
    out["system/licenses/newlib-COPYING.NEWLIB"] = (newlib / "COPYING.NEWLIB").read_bytes()
    lua_h = (fetch_deps.source_dir("lua") / "src" / "lua.h").read_text(errors="replace")
    notice = lua_h[lua_h.index("* Copyright (C) 1994"):]
    notice = notice[:notice.index("*/")]
    out["system/licenses/lua-LICENSE.txt"] = ("Lua 5.4.9 (https://www.lua.org), MIT license:\n\n" + "\n".join(
        line.lstrip("*").strip() for line in notice.splitlines()) + "\n").encode()
    # 0.7: TrueType fonts and the rasterizer's licence text.
    dejavu = fetch_deps.source_dir("dejavu")
    for font in ("DejaVuSans.ttf", "DejaVuSans-Bold.ttf", "DejaVuSansMono.ttf"):
        out["system/fonts/" + font] = (dejavu / "ttf" / font).read_bytes()
    out["system/licenses/dejavu-LICENSE.txt"] = (dejavu / "LICENSE").read_bytes()
    stb = (fetch_deps.file_dir("stb_truetype") / "stb_truetype.h").read_text(errors="replace")
    out["system/licenses/stb_truetype-LICENSE.txt"] = (
        "stb_truetype.h v1.26 (https://github.com/nothings/stb), used under the public-domain\n"
        "(Unlicense) alternative below:\n\n" + stb[stb.rindex("This software is available under 2 licenses"):]).encode()
    if with_tests and (tests / ".verified").exists():
        for f in sorted(tests.rglob("*")):
            if f.is_file() and f.name != ".verified" and f.suffix in (".lua", ".c", ".txt", ".md", ""):
                rel_p = f.relative_to(tests).as_posix()
                if all(part.replace(".", "").replace("_", "").replace("-", "").isalnum() for part in rel_p.split("/")):
                    out["system/share/lua-tests/" + rel_p] = f.read_bytes()
    return out
