#!/usr/bin/env python3
"""SMP and ACPI tests (0.5) against the real guest.

  boot-smp-N          boots with -smp 1, 2, 4 and 8; the guest reports N CPUs
                      online and every CPU's counters advance
  user-on-every-cpu   -smp 4: CPU-bound processes accumulate user time on all
                      four CPUs
  tlb-shootdown       -smp 4: a page unmapped on one CPU faults on another CPU
                      that had just read it; the control run without a
                      shootdown shows why the IPI is needed
  power-button        -smp 2: QMP system_powerdown (ACPI power button -> SCI)
                      shuts down cleanly and a file saved just before is intact
                      on the next boot
  stress              -smp 8: CPU-bound processes, parallel file writes with
                      checksums, TCP transfers to the host and window churn
                      for --stress-seconds (default 120; the 0.5 acceptance run
                      uses 1800). No panic, no lock-order violation, every
                      checksum and transfer verified.
Every scenario also requires zero LOCKDEP reports in the serial log.
Results: build/test-results/smp.json
"""
import argparse
import json
import random
import re
import socketserver
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.stdout.reconfigure(errors="replace")
import aurora_env as env  # noqa: E402
from qemu_harness import Guest, GuestError  # noqa: E402

OUT = env.BUILD / "test-results"
IMG = env.BUILD / "test" / "aurora.img"
results = []


class TcpEcho(socketserver.BaseRequestHandler):
    def handle(self):
        while True:
            d = self.request.recv(65536)
            if not d:
                return
            self.request.sendall(d)


def boot(smp, **kw):
    g = Guest(IMG, name=f"smp{smp}", smp=smp, keep_dir=OUT / f"smp-{smp}", **kw)
    g.start()
    g.wait_for(r"AURORA_DESKTOP_READY", timeout=300)
    g.key("ret")
    g.key("ctrl", "1")
    g.wait_for(r"@@WIN_OPEN id=\d+ app=Terminal", timeout=30)
    time.sleep(0.5)
    return g


def run(g, cmd, expect=None, timeout=30):
    since = g.mark()
    g.type_text(cmd + "\n")
    g.wait_for(r"@@TERM aurora:\S*\$ " + re.escape(cmd) + r"\s*$", timeout=timeout, since=since)
    return g.wait_for(expect, timeout=timeout, since=since) if expect else None


def cpu_rows(g, since):
    run(g, "cpus", r"@@CPUS online=\d+")
    rows = {}
    for m in re.finditer(r"@@CPU idx=(\d+) apic=\d+ online=(\d) ticks=(\d+) user=(\d+) idle=(\d+) switches=(\d+)",
                         g.serial()[since:]):
        rows[int(m.group(1))] = dict(online=m.group(2) == "1", ticks=int(m.group(3)), user=int(m.group(4)),
                                     idle=int(m.group(5)), switches=int(m.group(6)))
    return rows


def no_lockdep(g):
    text = g.serial()
    bad = [l for l in text.splitlines() if "LOCKDEP" in l]
    if bad:
        raise GuestError("lock validator reported: " + "; ".join(bad[:5]))
    m = list(re.finditer(r"@@CPUS online=\d+ lockdep_violations=(\d+) long_holds=(\d+)", text))
    if m and m[-1].group(1) != "0":
        raise GuestError(f"lockdep_violations={m[-1].group(1)}")


def check(name, fn, *a):
    t0 = time.time()
    try:
        detail = fn(*a)
        results.append({"test": name, "result": "pass", "detail": detail, "seconds": round(time.time() - t0, 1)})
        print(f"PASS  {name}: {detail}", flush=True)
    except GuestError as e:
        results.append({"test": name, "result": "fail", "detail": str(e).splitlines()[0], "seconds": round(time.time() - t0, 1)})
        print(f"FAIL  {name}: {str(e)[:1500]}", flush=True)


def t_boot(smp):
    g = boot(smp)
    try:
        m = g.wait_for(r"@@SMP cpus=(\d+) listed=(\d+) failed=(\d+)", timeout=5, since=0)
        if int(m.group(1)) != smp or m.group(3) != "0":
            raise GuestError(f"expected {smp} CPUs online: {m.group(0)}")
        since = g.mark()
        time.sleep(2)
        rows = cpu_rows(g, since)
        if len(rows) != smp or not all(r["online"] and r["ticks"] > 100 for r in rows.values()):
            raise GuestError(f"per-CPU counters: {rows}")
        no_lockdep(g)
        return f"{smp} CPU(s) online, every CPU's timer ticking ({', '.join(str(r['ticks']) for r in rows.values())} ticks)"
    finally:
        g.stop()


def t_user_everywhere():
    g = boot(4)
    try:
        for _ in range(6):
            run(g, "spin 0 &", r"@@PROC_START pid=\d+ name=spin")
        time.sleep(1)
        before = cpu_rows(g, g.mark())
        time.sleep(6)
        after = cpu_rows(g, g.mark())
        gained = {i: after[i]["user"] - before[i]["user"] for i in after}
        if len(gained) != 4 or min(gained.values()) < 50:
            raise GuestError(f"user ticks gained per CPU over 6 s: {gained}")
        for m in re.finditer(r"@@PROC_START pid=(\d+) name=spin", g.serial()):
            run(g, f"kill {m.group(1)}")
        no_lockdep(g)
        return "6 spin processes: user ticks gained per CPU " + ", ".join(f"cpu{i}={v}" for i, v in gained.items())
    finally:
        g.stop()


def t_tlb():
    g = boot(4)
    try:
        details = []
        for i in range(3):
            m = run(g, "tlbtest", r"@@TLBTEST flush=1 r=(-?\d+) unmap_cpu=(\d+) reader_cpu=(\d+) before=(\d) faulted=(\d) stale=(\d)")
            if m.group(1) != "0" or m.group(4) != "1" or m.group(5) != "1" or m.group(2) == m.group(3):
                raise GuestError(f"shootdown run {i}: {m.group(0)}")
            details.append(f"cpu{m.group(2)}->cpu{m.group(3)} fault")
        m = run(g, "tlbtest noflush", r"@@TLBTEST flush=0 r=(-?\d+) .* faulted=(\d) stale=(\d)")
        control = "stale translation seen" if m.group(3) == "1" else "no stale translation seen"
        no_lockdep(g)
        return f"3/3 shootdowns made the other CPU fault ({'; '.join(details)}); control without IPI: {control}"
    finally:
        g.stop()


def t_power_button():
    tmp = Path(tempfile.mkdtemp(prefix="aurora-smp-disk-"))
    disk = tmp / "data.img"
    with open(disk, "wb") as f:
        f.truncate(32 << 20)
    g = boot(2, data_disk=disk)
    try:
        g.wait_for(r"acpi: SCI on IRQ \d+, power button enabled", timeout=5, since=0)
        run(g, 'write /home/power.txt "saved before the power button"', r"@@TERM wrote")
        since = g.mark()
        g.command("system_powerdown")
        g.wait_for(r"acpi: power button pressed", timeout=20, since=since)
        g.wait_for(r"@@POWEROFF", timeout=20, since=since)
        t0 = time.time()
        while g.alive() and time.time() - t0 < 30:
            time.sleep(0.2)
        if g.alive():
            raise GuestError("QEMU still running 30 s after the power button")
        no_lockdep(g)
    finally:
        g.stop()
    g2 = boot(2, data_disk=disk)
    try:
        run(g2, "cat /home/power.txt", r"@@TERM saved before the power button")
        return "system_powerdown -> SCI -> sync + S5; QEMU exited; the file saved just before was intact after reboot"
    finally:
        g2.stop()


def t_stress(seconds):
    tcp = socketserver.ThreadingTCPServer(("127.0.0.1", 0), TcpEcho)
    tcp.daemon_threads = True
    threading.Thread(target=tcp.serve_forever, daemon=True).start()
    port = tcp.server_address[1]
    tmp = Path(tempfile.mkdtemp(prefix="aurora-smp-stress-"))
    disk = tmp / "data.img"
    with open(disk, "wb") as f:
        f.truncate(32 << 20)
    g = boot(8, data_disk=disk, network=True)
    rng = random.Random(5)
    try:
        g.wait_for(r"@@NET_UP ip=", timeout=60, since=0)
        for _ in range(4):
            run(g, "spin 0 &", r"@@PROC_START pid=\d+ name=spin")
        deadline = time.time() + seconds
        fs_runs = tcp_runs = churn = 0
        fs_ids = 0
        while time.time() < deadline:
            # parallel file writers
            for _ in range(3):
                fs_ids += 1
                # directory ids cycle so the disk's 512-entry catalog never fills
                run(g, f"fsstress {(fs_ids - 1) % 24 + 1} 25 &", r"@@PROC_START pid=\d+ name=fsstress")
            fs_runs += 3
            # a TCP transfer to the host
            run(g, f"tcpecho 10.0.2.2 {port} 30000 &", r"@@PROC_START pid=\d+ name=tcpecho")
            tcp_runs += 1
            # window churn: open and close apps while the work runs
            for _ in range(3):
                app = rng.choice(["2", "3", "4"])
                since = g.mark()
                g.key("ctrl", app)
                g.wait_for(r"@@WIN_OPEN id=\d+|@@FOCUS", timeout=20, since=since)
                time.sleep(0.3)
                g.key("ctrl", "w")
                time.sleep(0.3)
                churn += 1
            g.key("ctrl", "1")  # back to the Terminal
            time.sleep(0.5)
            # wait for this batch to finish before starting the next
            t_batch = time.time()
            while time.time() - t_batch < 120:
                text = g.serial()
                done_fs = len(re.findall(r"@@PROC_EXIT pid=\d+ name=fsstress", text))
                done_tcp = len(re.findall(r"@@PROC_EXIT pid=\d+ name=tcpecho", text))
                if done_fs >= fs_runs and done_tcp >= tcp_runs:
                    break
                if not g.alive() or "AURORA_PANIC" in text:
                    raise GuestError("guest died during stress")
                time.sleep(1)
            else:
                raise GuestError(f"batch did not finish: fsstress {fs_runs}, tcpecho {tcp_runs}")
        text = g.serial()
        fs_ok = re.findall(r"@@TERM fsstress \d+: 25 rounds, (\d+) bytes verified, 0 errors", text)
        fs_bad = re.findall(r"@@TERM fsstress \d+: (MISMATCH.*|.*[1-9]\d* errors.*)", text)
        tcp_ok = re.findall(r"@@TERM tcpecho: 30000 bytes echoed, identical", text)
        exits = re.findall(r"@@PROC_EXIT pid=\d+ name=(fsstress|tcpecho) status=(-?\d+)", text)
        bad_exit = [e for e in exits if e[1] != "0"]
        if fs_bad or len(fs_ok) != fs_runs or len(tcp_ok) != tcp_runs or bad_exit:
            raise GuestError(f"fsstress ok {len(fs_ok)}/{fs_runs} bad {fs_bad[:3]}; tcp ok {len(tcp_ok)}/{tcp_runs}; "
                             f"bad exits {bad_exit[:5]}")
        rows = cpu_rows(g, g.mark())
        if len(rows) != 8:
            raise GuestError(f"expected 8 CPUs, saw {len(rows)}")
        idle_cpus = [i for i, r in rows.items() if r["user"] == 0]
        if idle_cpus:
            raise GuestError(f"CPUs that never ran user code: {idle_cpus}")
        no_lockdep(g)
        total = sum(int(b) for b in fs_ok)
        return (f"{seconds} s at -smp 8: {fs_runs} fsstress runs ({total} bytes verified), {tcp_runs} TCP transfers "
                f"of 30000 B identical, {churn} window open/close cycles, 4 CPU hogs; no panic, no lockdep report")
    finally:
        g.stop()
        tcp.shutdown()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--stress-seconds", type=int, default=120)
    args = ap.parse_args()
    if not IMG.exists():
        sys.exit("error: build/test/aurora.img missing; run 'python tools/build.py build --variant test'")
    OUT.mkdir(parents=True, exist_ok=True)
    plan = [(f"boot-smp-{n}", t_boot, n) for n in (1, 2, 4, 8)]
    plan += [("user-on-every-cpu", t_user_everywhere), ("tlb-shootdown", t_tlb), ("power-button", t_power_button),
             ("stress", t_stress, args.stress_seconds)]
    for name, fn, *a in plan:
        if args.only and name not in args.only:
            continue
        check(name, fn, *a)
    (OUT / "smp.json").write_text(json.dumps(results, indent=2))
    failed = [r for r in results if r["result"] == "fail"]
    print(f"smp: {len(results) - len(failed)}/{len(results)} passed")
    sys.exit(1 if failed or not results else 0)


if __name__ == "__main__":
    main()
