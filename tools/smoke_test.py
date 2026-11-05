#!/usr/bin/env python3
"""Real-guest smoke tests (QEMU, headless, software emulation).

1. Release image boots at 128, 256 and 512 MiB (UEFI) and reaches the
   desktop; milestone markers are checked in order.
2. Release image boots via SeaBIOS (if the image has Limine's BIOS stage).
3. Panic test build: a deliberate NULL write produces a page-fault panic
   report with vector, error code, RIP and CR2, then halts (no reset).

Writes results to build/test-results/smoke.json and keeps serial logs.
"""
import json
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import aurora_env as env  # noqa: E402
from qemu_harness import Guest, GuestError  # noqa: E402

OUT = env.BUILD / "test-results"
MILESTONES = ["serial", "bootinfo", "framebuffer", "gdt_idt", "paging", "memory", "interrupts", "vfs"]
results = []


def record(name, ok, detail="", seconds=0.0):
    results.append({"test": name, "result": "pass" if ok else "fail", "detail": detail, "seconds": round(seconds, 1)})
    print(f"{'PASS' if ok else 'FAIL'}  {name}  {detail}")


def boot(name, image, memory="256M", firmware="uefi", shot=None):
    t0 = time.time()
    g = Guest(image, memory=memory, firmware=firmware, name=name, keep_dir=OUT / name)
    try:
        g.start()
        g.wait_for(r"AURORA_DESKTOP_READY", timeout=180)
        text = g.serial()
        pos = [text.find(f"AURORA_MILESTONE {m}") for m in MILESTONES]
        order_ok = all(p >= 0 for p in pos) and pos == sorted(pos)
        usable = re.search(r"pmm: \d+ pages tracked, (\d+) usable \((\d+) MiB\)", text)
        detail = f"usable={usable.group(2)}MiB" if usable else ""
        if shot:
            time.sleep(1.0)
            g.screendump(OUT / shot)
        # The guest must still be alive a few seconds later (no reset/panic).
        time.sleep(3)
        alive = g.alive() and "AURORA_PANIC" not in g.serial()
        record(name, order_ok and alive, detail + ("" if order_ok else " milestones out of order"), time.time() - t0)
    except GuestError as e:
        record(name, False, str(e).splitlines()[0], time.time() - t0)
    finally:
        g.stop()


def panic_test():
    t0 = time.time()
    img = env.BUILD / "panic" / "aurora.img"
    g = Guest(img, name="panic-test", keep_dir=OUT / "panic-test")
    try:
        g.start()
        g.wait_for(r"System halted\.", timeout=180, fail_on_panic=False)
        text = g.serial()
        report = text[text.find("*** AURORA_PANIC ***"):]
        checks = {
            "vector": "CPU exception 14: Page fault" in report,
            "cr2": re.search(r"CR2=0000000000000000", report) is not None,
            "error_code": "error_code=0000000000000002" in report,
            "rip": re.search(r"rip=ffffffff8[0-9a-f]{7}", report) is not None,
            "registers": "rax=" in report and "r15=" in report and "cr3=" in report,
            "no_desktop": "AURORA_DESKTOP_READY" not in text,
        }
        g.screendump(OUT / "panic.png")
        time.sleep(2)
        checks["halted_not_reset"] = g.alive() and text.count("AURORA_MILESTONE serial") == 1
        ok = all(checks.values())
        record("panic-diagnostic", ok, ", ".join(k for k, v in checks.items() if not v) or "page fault report complete",
               time.time() - t0)
    except GuestError as e:
        record("panic-diagnostic", False, str(e).splitlines()[0], time.time() - t0)
    finally:
        g.stop()


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    img = env.BUILD / "release" / "aurora.img"
    if not img.exists():
        sys.exit("error: build/release/aurora.img missing; run 'make' first")
    for mem in ("128M", "256M", "512M"):
        boot(f"boot-uefi-{mem}", img, memory=mem, shot="desktop-uefi-256M.png" if mem == "256M" else None)
    if (env.BUILD / "release" / "bios.txt").exists() and (env.BUILD / "release" / "bios.txt").read_text().strip() == "yes":
        boot("boot-bios-256M", img, firmware="bios")
    else:
        record("boot-bios-256M", False, "image has no BIOS stage (limine bios-install unavailable)")
    if (env.BUILD / "panic" / "aurora.img").exists():
        panic_test()
    else:
        record("panic-diagnostic", False, "build/panic/aurora.img missing")
    (OUT / "smoke.json").write_text(json.dumps(results, indent=2))
    failed = [r for r in results if r["result"] != "pass"]
    print(f"smoke: {len(results) - len(failed)}/{len(results)} passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
