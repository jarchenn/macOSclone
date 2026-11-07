#!/usr/bin/env python3
"""Persistent storage tests (0.3) against the real guest with a virtio data
disk: durability across reboot and power loss, crash consistency under a
commit-heavy stress load with repeated hard kills, and handling of damaged or
blank disks.

Every scenario uses its own fresh raw disk image in a temp directory.
Results: build/test-results/storage.json
"""
import json
import os
import random
import re
import sys
import tempfile
import time
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.stdout.reconfigure(errors="replace")  # guest output may contain any bytes
import aurora_env as env  # noqa: E402
from qemu_harness import Guest, GuestError  # noqa: E402

OUT = env.BUILD / "test-results"
IMG = env.BUILD / "test" / "aurora.img"
results = []


def new_disk(tmp, name="data.img", mib=32):
    p = Path(tmp) / name
    with open(p, "wb") as f:
        f.truncate(mib * 1024 * 1024)
    return p


class Session:
    def __init__(self, disk, name):
        self.g = Guest(IMG, data_disk=str(disk), name=name, no_reboot=False, keep_dir=OUT / name)

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

    def run(self, cmd, expect=None, timeout=20):
        since = self.g.mark()
        self.g.type_text(cmd + "\n")
        self.g.wait_for(r"@@TERM aurora:\S*\$ " + re.escape(cmd) + r"\s*$", timeout=timeout, since=since)
        if expect:
            return self.g.wait_for(expect, timeout=timeout, since=since)
        return None

    def kill(self):
        """Power cut: QEMU is killed without any guest shutdown."""
        self.g.proc.kill()
        self.g.proc.wait(timeout=10)

    def __exit__(self, *a):
        self.g.stop()


def crc(s):
    return f"{zlib.crc32(s.encode()) & 0xFFFFFFFF:08x}"


def scenario(fn):
    def wrapped():
        t0 = time.time()
        try:
            detail = fn()
            results.append({"test": fn.__name__, "result": "pass", "detail": detail, "seconds": round(time.time() - t0, 1)})
            print(f"PASS  {fn.__name__}: {detail}")
        except (GuestError, AssertionError) as e:
            results.append({"test": fn.__name__, "result": "fail", "detail": str(e).splitlines()[0], "seconds": round(time.time() - t0, 1)})
            print(f"FAIL  {fn.__name__}: {str(e)[:1500]}")
    wrapped.__name__ = fn.__name__
    return wrapped


@scenario
def persist_across_reboot():
    with tempfile.TemporaryDirectory() as tmp:
        disk = new_disk(tmp)
        text = 'two  spaces\\nand \\"quotes\\", line 2!'  # typed with Terminal escapes
        expected = 'two  spaces\nand "quotes", line 2!'
        with Session(disk, "storage-reboot") as s:
            s.g.wait_for(r"vfs: /home is on the data disk \(blank disk formatted\)", timeout=5, since=0)
            s.run(f'write /home/notes/keep.txt "{text}"', r"@@TERM wrote \d+ bytes")
            s.run("mkdir /home/projects")
            s.run("write /home/projects/plan.txt step-one")
            s.run("mv /home/projects/plan.txt /home/plan.txt")
            s.run("rm /home/notes/example.txt")
            since = s.g.mark()
            s.g.type_text("reboot\n")
            s.g.wait_for(r"@@REBOOT", timeout=10, since=since)
            s.g.wait_for(r"AURORA_MILESTONE serial", timeout=120, since=since)
            s.boot(since=since)
            s.g.wait_for(r"vfs: /home is on the data disk \(existing files loaded\)", timeout=5, since=since)
            m = s.run("cat /home/notes/keep.txt", r"@@CAT path=/home/notes/keep.txt len=(\d+) crc=([0-9a-f]+)")
            if m.group(2) != crc(expected):
                raise GuestError(f"content changed across reboot: {m.group(0)}")
            s.run("cat /home/plan.txt", r"@@CAT path=/home/plan.txt len=8 crc=" + crc("step-one"))
            s.run("cat /home/notes/example.txt", r"@@CAT_ERR path=/home/notes/example.txt err=-1")
            s.run("ls /home/projects", r"@@TERM \(empty directory\)")
            s.run("df", r"@@TERM /home  data disk")
    return "file contents, mkdir, mv and rm all survived an 8042 reset"


@scenario
def notes_save_survives_power_loss():
    with tempfile.TemporaryDirectory() as tmp:
        disk = new_disk(tmp)
        note = "Saved with Ctrl+S\nthen the power went out."
        with Session(disk, "storage-powerloss-1") as s:
            since = s.g.mark()
            s.g.key("ctrl", "3")  # Notes
            s.g.wait_for(r"@@WIN_OPEN id=\d+ app=Notes", timeout=10, since=since)
            time.sleep(0.3)
            s.g.type_text(note)
            since = s.g.mark()
            s.g.key("ctrl", "s")
            s.g.wait_for(r"@@NOTES_DIALOG saveas", timeout=10, since=since)
            for _ in range(len("untitled.txt")):
                s.g.key("backspace")
            s.g.type_text("power.txt\n")
            m = s.g.wait_for(r"@@NOTES_SAVE path=/home/notes/power.txt result=(-?\d+) len=(\d+) crc=([0-9a-f]+)", timeout=10, since=since)
            if m.group(1) != "0":
                raise GuestError(f"save failed: {m.group(0)}")
            s.g.wait_for(r"@@NOTES_STATUS err=0 msg=Saved /home/notes/power.txt \(\d+ bytes, on disk\)", timeout=5, since=since)
            s.kill()  # immediately after the save returned
        with Session(disk, "storage-powerloss-2") as s:
            s.run("cat /home/notes/power.txt", r"@@CAT path=/home/notes/power.txt len=\d+ crc=" + crc(note))
    return "a note saved in Notes was intact after QEMU was killed right after the save"


@scenario
def crash_consistency_under_stress():
    """Kill QEMU at random points while files are being rewritten and
    committed continuously; every surviving file must be a complete version
    and every round reported as finished must be durable."""
    rnd = random.Random(20261106)
    cycles, rounds_seen = 4, []
    with tempfile.TemporaryDirectory() as tmp:
        disk = new_disk(tmp)
        last_done = 0
        for cycle in range(cycles):
            with Session(disk, f"storage-stress-{cycle}") as s:
                if cycle:
                    m = s.run("pfscheck", r"@@PFS_CHECK ok=(\d+) bad=(\d+) missing=(\d+) min=(\d+) max=(\d+)")
                    ok, bad, missing, lo, hi = map(int, m.groups())
                    if bad:
                        raise GuestError(f"cycle {cycle}: {bad} torn/mixed files after power loss")
                    if ok + missing != 8 or ok == 0:
                        raise GuestError(f"cycle {cycle}: unexpected check result {m.group(0)}")
                    if lo < last_done:
                        raise GuestError(f"cycle {cycle}: round {last_done} was reported complete but disk has round {lo}")
                    rounds_seen.append((lo, hi))
                since = s.g.mark()
                s.g.type_text("pfsstress 400\n")
                target = rnd.randint(3, 12)
                m = s.g.wait_for(rf"@@PFS_ROUND {target}\b", timeout=180, since=since)
                time.sleep(rnd.random() * 0.4)  # land somewhere inside a later round
                text = s.g.serial()[since:]
                done = [int(x) for x in re.findall(r"@@PFS_ROUND (\d+)", text)]
                last_done = max(done) if done else 0
                s.kill()
        with Session(disk, "storage-stress-final") as s:
            m = s.run("pfscheck", r"@@PFS_CHECK ok=(\d+) bad=(\d+) missing=(\d+) min=(\d+) max=(\d+)")
            ok, bad, missing, lo, hi = map(int, m.groups())
            if bad or ok != 8 or lo < last_done:
                raise GuestError(f"final check failed: {m.group(0)} (last reported round {last_done})")
            s.run("df", r"@@TERM /home  data disk")
    return f"{cycles} hard kills mid-commit; every file complete, reported rounds durable (rounds seen {rounds_seen})"


@scenario
def damaged_disk_is_not_reformatted():
    with tempfile.TemporaryDirectory() as tmp:
        disk = new_disk(tmp)
        with Session(disk, "storage-damage-1") as s:
            s.run("write /home/important.txt do-not-lose", r"@@TERM wrote")
        # Corrupt both superblock slots (blocks 0 and 1) but keep the magic.
        data = bytearray(Path(disk).read_bytes())
        for blk in (0, 1):
            off = blk * 4096
            data[off + 20] ^= 0xFF
            data[off + 44] ^= 0xFF
        Path(disk).write_bytes(bytes(data))
        with Session(disk, "storage-damage-2") as s:
            s.g.wait_for(r"vfs: warning: data disk not used \(disk I/O error\); /home stays in RAM", timeout=5, since=0)
            s.run("df", r"@@TERM /home  RAM")
        after = Path(disk).read_bytes()
        if after[:8] != b"AURPFS01" and after[4096:4104] != b"AURPFS01":
            raise GuestError("the damaged disk was overwritten")
    return "a disk with damaged superblocks was left untouched and /home fell back to RAM"


@scenario
def files_and_terminal_show_disk_state():
    with tempfile.TemporaryDirectory() as tmp:
        disk = new_disk(tmp)
        with Session(disk, "storage-ui") as s:
            s.run("about", r"@@TERM Files in /home are saved to the data disk and survive restarts.")
            s.run("mem", r"@@TERM Data disk: +\d+ of 32768 KiB used")
            s.run("sync", r"@@TERM sync: all changes are on the data disk")
            s.run("run cat /home/notes/example.txt", r"@@TERM With a data disk attached")
    return "about/mem/sync report the data disk; user programs read /home from it"


def main():
    if not IMG.exists():
        sys.exit("error: build/test/aurora.img missing; run 'python tools/build.py build --variant test'")
    OUT.mkdir(parents=True, exist_ok=True)
    only = sys.argv[1:]
    for fn in (persist_across_reboot, notes_save_survives_power_loss, crash_consistency_under_stress,
               damaged_disk_is_not_reformatted, files_and_terminal_show_disk_state):
        if only and fn.__name__ not in only:
            continue
        fn()
    (OUT / "storage.json").write_text(json.dumps(results, indent=2))
    failed = [r for r in results if r["result"] != "pass"]
    print(f"storage: {len(results) - len(failed)}/{len(results)} passed")
    sys.exit(1 if failed or not results else 0)


if __name__ == "__main__":
    main()
