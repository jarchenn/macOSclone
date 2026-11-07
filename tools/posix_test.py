#!/usr/bin/env python3
"""POSIX userland tests (0.6) against the real guest: the roadmap's
acceptance tests for 0.6, each driving programs from the desktop Terminal.

  posix-c-program        printf/malloc/file I/O/mmap/pthreads (posixtest)
  pipeline-count         cat file | grep x | wc -l > out gives the right count
  ctrl-c-shell-survives  Ctrl+C kills the foreground program, sh survives
  segv-child-waitpid     a segfaulting child is reported via waitpid (proctest)
  fork-1000              1000 fork/exit cycles restore the free-page count
  sigreturn-registers    handlers clobbering every register (sigtest)
  permissions            non-root cannot open a root-owned 0600 file (permtest)
  vt100-terminal         CR, backspace and CSI K/C/D in the Terminal
  rename-unlink-reboot   rename/unlink results persist across a clean reboot
  lua-test-suite         Lua 5.4.9's own test suite (portable subset, _U)

Set AURORA_TEST_SMP=N to run every scenario with N virtual CPUs.
Results: build/test-results/posix.json
"""
import json
import re
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.stdout.reconfigure(errors="replace")
import aurora_env as env  # noqa: E402
import fetch_deps  # noqa: E402
from qemu_harness import Guest, GuestError  # noqa: E402

OUT = env.BUILD / "test-results"
IMG = env.BUILD / "test" / "aurora.img"
results = []


class Session:
    def __init__(self, name, disk=None, memory="512M"):
        self.g = Guest(IMG, data_disk=str(disk) if disk else None, name=name, no_reboot=disk is None,
                       keep_dir=OUT / name, memory=memory)

    def __enter__(self):
        self.g.start()
        self.boot()
        return self

    def boot(self, since=None):
        self.g.wait_for(r"AURORA_DESKTOP_READY", timeout=180, since=since)
        self.g.key("ret")  # dismiss Welcome
        time.sleep(0.3)
        s = self.g.mark()
        self.g.key("ctrl", "1")
        self.g.wait_for(r"@@WIN_OPEN id=\d+ app=Terminal", timeout=20, since=s)
        time.sleep(0.3)

    def run(self, cmd, expect=None, timeout=30):
        """A Terminal command line; returns the match of `expect`."""
        since = self.g.mark()
        self.g.type_text(cmd + "\n")
        self.g.wait_for(r"@@TERM aurora:\S*\$ " + re.escape(cmd) + r"\s*$", timeout=timeout, since=since)
        if expect:
            return self.g.wait_for(expect, timeout=timeout, since=since)
        return None

    def program(self, cmd, name, ok_line, timeout=120):
        """Runs a program and requires ok_line plus exit status 0."""
        since = self.g.mark()
        self.run(cmd)
        m = self.g.wait_for(ok_line, timeout=timeout, since=since)
        self.g.wait_for(rf"@@PROC_EXIT pid=\d+ name={name} status=0 ", timeout=timeout, since=since)
        return m

    def __exit__(self, *a):
        self.g.stop()


def scenario(fn):
    def wrapped():
        t0 = time.time()
        try:
            detail = fn()
            results.append({"name": fn.__name__.replace("_", "-"), "ok": True, "detail": detail,
                            "seconds": round(time.time() - t0, 1)})
            print(f"PASS  {fn.__name__.replace('_', '-')}: {detail}", flush=True)
        except Exception as e:  # noqa: BLE001
            results.append({"name": fn.__name__.replace("_", "-"), "ok": False, "detail": str(e)[:2000],
                            "seconds": round(time.time() - t0, 1)})
            print(f"FAIL  {fn.__name__.replace('_', '-')}: {str(e)[:2000]}", flush=True)
    wrapped.__name__ = fn.__name__
    return wrapped


def no_lockdep(s):
    if "LOCKDEP" in s.g.serial():
        raise GuestError("lock validator report in the serial log")


@scenario
def posix_c_program():
    with Session("posix-c") as s:
        s.program("posixtest", "posixtest", r"@@TERM posixtest: 17/17 passed")
        no_lockdep(s)
    return "posixtest 17/17: printf (incl. floats), malloc, stdio/POSIX file I/O, mmap, 8 pthreads + mutex + condvar + TLS"


@scenario
def pipeline_count():
    tests = fetch_deps.source_dir("lua-tests") / "all.lua"
    want = sum(1 for line in tests.read_text(errors="replace").splitlines() if "dofile" in line)
    with Session("posix-pipeline") as s:
        since = s.g.mark()
        # The echoed line wraps at the window width, so wait for the shell
        # process instead of the echo.
        s.g.type_text("cat /system/share/lua-tests/all.lua | grep dofile | wc -l > /home/count.txt\n")
        s.g.wait_for(r"@@PROC_START pid=\d+ name=sh", timeout=30, since=since)
        s.g.wait_for(r"@@PROC_EXIT pid=\d+ name=sh status=0 ", timeout=30, since=since)
        execs = re.findall(r"@@PROC_EXEC pid=\d+ name=(\w+)", s.g.serial()[since:])
        m = s.run("cat /home/count.txt", r"@@TERM (\d+)\s*$")
        if int(m.group(1)) != want:
            raise GuestError(f"pipeline counted {m.group(1)}, host counts {want}")
        if sorted(execs) != ["cat", "grep", "wc"]:
            raise GuestError(f"pipeline ran {execs}")
    return f"cat | grep dofile | wc -l > /home/count.txt = {want} (host count {want}); three exec'd processes"


@scenario
def ctrl_c_shell_survives():
    with Session("posix-ctrlc") as s:
        since = s.g.mark()
        s.run("sh", r"@@TERM Aurora sh \(pid (\d+)\)")
        sh_pid = re.search(r"@@PROC_START pid=(\d+) name=sh", s.g.serial()[since:]).group(1)
        time.sleep(0.5)
        since = s.g.mark()
        s.g.type_text("spin\n")
        s.g.wait_for(r"@@TERM spin: \d+ M iterations", timeout=20, since=since)
        s.g.key("ctrl", "c")
        s.g.wait_for(r"@@PTY_SIGNAL pty=\d+ sig=2 pgrp=" + sh_pid, timeout=10, since=since)
        s.g.wait_for(r"@@PROC_EXIT pid=\d+ name=spin status=2 reason=killed by SIGINT", timeout=10, since=since)
        since = s.g.mark()
        s.g.type_text("echo shell-alive $$\n")
        s.g.wait_for(rf"@@TERM shell-alive {sh_pid}\s*$", timeout=10, since=since)
        if re.search(rf"@@PROC_EXIT pid={sh_pid} ", s.g.serial()):
            raise GuestError("the shell died")
        s.g.type_text("exit\n")
        s.g.wait_for(rf"@@PROC_EXIT pid={sh_pid} name=sh status=0", timeout=10, since=since)
    return f"Ctrl+C sent SIGINT to process group {sh_pid}: spin died (status 2), sh (pid {sh_pid}) kept running"


@scenario
def segv_child_waitpid():
    with Session("posix-proc") as s:
        s.program("proctest", "proctest", r"@@TERM proctest: 15/15 passed")
        m = re.search(r"@@TERM proctest: child \d+ status (0x[0-9a-f]+)", s.g.serial())
        no_lockdep(s)
    return (f"child segfault -> waitpid status {m.group(1)} (WTERMSIG = SIGSEGV); also exec/cloexec, posix_spawn, "
            "orphan -> init, pipes (EOF, EPIPE, SIGPIPE, EAGAIN, poll), dup/fcntl: 15/15")


@scenario
def fork_1000():
    with Session("posix-fork") as s:
        m = s.program("forktest 1000", "forktest",
                      r"@@TERM forktest: 1000 cycles, free pages before (\d+) after (\d+) \(baseline restored\)",
                      timeout=900)
        no_lockdep(s)
    return f"1000 fork/exit cycles: free pages {m.group(1)} -> {m.group(2)}"


@scenario
def sigreturn_registers():
    with Session("posix-signals") as s:
        s.program("sigtest", "sigtest", r"@@TERM sigtest: 5/5 passed", timeout=300)
        n = re.search(r"@@TERM sigtest: (\d+) signals during the computation", s.g.serial()).group(1)
        no_lockdep(s)
    return (f"{n} SIGUSR1 handlers clobbered all GPRs + xmm0-15 mid-computation, result unchanged; pending/unblock, "
            "EINTR, SA_RESTART, SIGTERM status")


@scenario
def permissions():
    with Session("posix-perm") as s:
        s.program("permtest", "permtest", r"@@TERM permtest: all checks passed")
    return "uid 1000 gets EACCES on a root 0600 file (read/write/access), EACCES exec, EPERM chmod/setuid"


@scenario
def vt100_terminal():
    with Session("posix-vt100") as s:
        s.run("sh", r"@@TERM Aurora sh")
        time.sleep(0.5)
        checks = [
            ("printf 'abcdef\\rXY\\e[K\\n'", r"@@TERM XY\s*$"),
            ("printf 'one\\btwo\\n'", r"@@TERM ontwo\s*$"),
            ("printf 'abc\\e[2DZ\\n'", r"@@TERM aZc\s*$"),
            ("printf 'x\\e[3Cy\\n'", r"@@TERM x   y\s*$"),
            ("printf 'tab\\there\\n'", r"@@TERM tab     here\s*$"),
        ]
        for cmd, pat in checks:
            since = s.g.mark()
            s.g.type_text(cmd + "\n")
            s.g.wait_for(pat, timeout=10, since=since)
        s.g.type_text("exit\n")
    return "CR + erase-line, backspace, cursor left/right (CSI D/C) and tabs render as on a VT100"


@scenario
def rename_unlink_reboot():
    with tempfile.TemporaryDirectory() as tmp:
        disk = Path(tmp) / "data.img"
        with open(disk, "wb") as f:
            f.truncate(32 * 1024 * 1024)
        with Session("posix-rename", disk=disk) as s:
            s.program("rentest setup", "rentest", r"@@TERM rentest: setup done")
            since = s.g.mark()
            s.g.type_text("reboot\n")
            s.g.wait_for(r"@@REBOOT", timeout=10, since=since)
            s.g.wait_for(r"AURORA_MILESTONE serial", timeout=120, since=since)
            s.boot(since=since)
            s.g.wait_for(r"vfs: /home is on the data disk \(existing files loaded\)", timeout=5, since=since)
            s.program("rentest check", "rentest", r"@@TERM rentest: check passed")
    return "rename (incl. over an existing file), unlink, mkdir/rmdir, truncate all persisted across a reboot"


@scenario
def lua_test_suite():
    with Session("posix-lua", memory="1G") as s:
        s.run("cd /system/share/lua-tests")
        since = s.g.mark()
        s.run("lua -e _U=true all.lua")
        m = s.g.wait_for(r"@@TERM final OK !!!", timeout=1800, since=since)
        s.g.wait_for(r"@@PROC_EXIT pid=\d+ name=lua status=0 ", timeout=60, since=since)
        t = re.search(r"@@TERM total time: ([\d.]+)s", s.g.serial()[since:])
        no_lockdep(s)
    return f"Lua 5.4.9 test suite (_U: portable, non-heavy subset) printed 'final OK !!!' in {t.group(1) if t else '?'} s"


ALL = [posix_c_program, pipeline_count, ctrl_c_shell_survives, segv_child_waitpid, fork_1000, sigreturn_registers,
       permissions, vt100_terminal, rename_unlink_reboot, lua_test_suite]


def main():
    names = sys.argv[1:]
    OUT.mkdir(parents=True, exist_ok=True)
    for fn in ALL:
        if names and fn.__name__.replace("_", "-") not in names:
            continue
        fn()
    ok = sum(r["ok"] for r in results)
    print(f"posix: {ok}/{len(results)} passed", flush=True)
    (OUT / "posix.json").write_text(json.dumps(results, indent=2))
    return 0 if ok == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
