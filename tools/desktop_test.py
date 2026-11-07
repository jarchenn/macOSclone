#!/usr/bin/env python3
"""0.7 acceptance tests (desktop maturity), on the `test` build in QEMU:

  golden-text          Settings > Appearance (text at 11, 13, 15 and 24 px) matches its golden image
  resize-200-1600      Notes resized from 200x150 to 1600x1000 content (at 1920x1200): each size is
                       acknowledged at the new size, matches its golden image, and nothing is left behind
  golden-dialogs       the save and open dialogs match their golden images
  save-dialog-roundtrip  a note saved through the save dialog (into a folder made with New Folder)
                       reopens through the open dialog with identical bytes
  clipboard-after-exit text copied in Notes pastes into the Terminal after Notes has exited
  dead-keys-caps       us-intl: ' e -> e acute, " u -> u diaeresis; Caps Lock sets and clears its LED
  wheel                the mouse wheel scrolls the Terminal
  resolution-relayout  1024x768 -> 800x600 from Settings re-lays out every open window
  kill-app             killing an app leaves the display server and the other apps running
  kill-display         killing the display server leaves the kernel running; init restarts it and the
                       session works again

Golden images live in tests/golden/ (PNG). --update-goldens rewrites them from this run; they must be
inspected by eye before they are committed.

Usage: desktop_test.py [--only NAME ...] [--update-goldens]
Results: build/test-results/desktop.json and screenshots in build/test-results/desktop/.
"""
import argparse
import json
import re
import sys
import time
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.stdout.reconfigure(errors="replace")
import aurora_env as env  # noqa: E402
import imgdiff  # noqa: E402
from qemu_harness import Guest, GuestError  # noqa: E402
from interaction_test import Desktop, TITLE_H  # noqa: E402

OUT = env.BUILD / "test-results"
SHOTS = OUT / "desktop"
IMG = env.BUILD / "test" / "aurora.img"
GOLDEN = env.ROOT / "tests" / "golden"
TOL = 8              # largest per-channel difference that still counts as equal
MAX_DIFF = 0.001     # fraction of pixels allowed to differ (anti-aliasing noise)
MIN_SLACK = 64       # ...but at least this many: a text caret (2x18 px) may blink between frames
UPDATE = False


def crc(s: str) -> str:
    return f"{zlib.crc32(s.encode()) & 0xFFFFFFFF:08x}"


def refresh(d):
    """Re-read the screen size and dock after a resolution change."""
    text = d.g.serial()
    m = list(re.finditer(r"@@SCREEN w=(\d+) h=(\d+)", text))[-1]
    d.screen = (int(m.group(1)), int(m.group(2)))
    tail = text[m.start():]
    for mm in re.finditer(r"@@DOCK_ITEM app=(\w+) x=(\d+) y=(\d+) w=(\d+) h=(\d+)", tail):
        d.dock[mm.group(1)] = (int(mm.group(2)) + int(mm.group(4)) // 2, int(mm.group(3)) + int(mm.group(5)) // 2)
    c = re.findall(r"@@CURSOR x=(\d+) y=(\d+)", tail)
    if c:
        d.cursor = (int(c[-1][0]), int(c[-1][1]))
    d.cursor = (min(d.cursor[0], d.screen[0] - 1), min(d.cursor[1], d.screen[1] - 1))


def park_cursor(d):
    """Cursor to the bottom-left corner, out of every compared region."""
    d.move_to(2, d.screen[1] - 2)


def shot(d, name):
    path = SHOTS / f"{name}.ppm"
    d.g.screendump_ppm(path)
    img = imgdiff.read_ppm(path)
    imgdiff.write_png(SHOTS / f"{name}.png", img)
    path.unlink()
    return img


def golden(d, name, region, mask=()):
    """Compare region (x, y, w, h) of the screen with tests/golden/<name>.png."""
    park_cursor(d)
    time.sleep(0.6)  # two frames at most
    img = shot(d, name).crop(*region)
    gpath = GOLDEN / f"{name}.png"
    if UPDATE or not gpath.exists():
        if not UPDATE:
            raise GuestError(f"golden image {gpath.name} missing (run with --update-goldens and inspect it)")
        GOLDEN.mkdir(parents=True, exist_ok=True)
        imgdiff.write_png(gpath, img)
        return f"{name}: golden written ({img.w}x{img.h})"
    want = imgdiff.read_png(gpath)
    bad, total, worst = imgdiff.diff(img, want, TOL, mask)
    imgdiff.write_png(SHOTS / f"{name}-region.png", img)
    if bad > max(MIN_SLACK, total * MAX_DIFF):
        raise GuestError(f"{name}: {bad} of {total} pixels differ from the golden image (largest delta {worst})")
    return f"{name}: {bad}/{total} px differ (max delta {worst})"


def window(d, app):
    return d.window(app)


def wait_ack(d, app, w, h, since, timeout=20):
    return d.g.wait_for(rf"@@WIN_ACK id=\d+ app={app} serial=\d+ w={w} h={h} match=1", timeout=timeout, since=since)


def term(d, line, expect=None, timeout=15):
    return d.terminal(line, expect, timeout)


def term_focus(d):
    since = d.g.mark()
    d.dock_click("Terminal")
    d.wait_focus("Terminal", since)


def open_notes_fresh(d):
    since = d.g.mark()
    d.dock_click("Notes")
    d.wait_focus("Notes", since)
    since = d.g.mark()
    d.g.key("ctrl", "n")
    d.g.wait_for(r"@@NOTES_STATUS err=0 msg=New note", timeout=10, since=since)


def set_mode(d, w, h):
    term_focus(d)
    since = d.g.mark()
    term(d, f"display {w}x{h}")
    d.g.wait_for(rf"@@SCREEN w={w} h={h}", timeout=20, since=since)
    time.sleep(0.5)
    refresh(d)


def close_notes_discard(d):
    since = d.g.mark()
    d.g.key("ctrl", "w")
    m = d.g.wait_for(r"@@(NOTES_DIALOG unsaved|WIN_CLOSE id=\d+)", timeout=10, since=since)
    if "unsaved" in m.group(0):
        d.g.key("d")
        d.g.wait_for(r"@@WIN_CLOSE id=\d+", timeout=10, since=since)


def layout(d, since):
    m = d.g.wait_for(r"@@DIALOG_LAYOUT up=(\d+),(\d+) newfolder=(\d+),(\d+) ok=(\d+),(\d+) cancel=(\d+),(\d+) "
                     r"name=(-?\d+),(-?\d+) list=(\d+),(\d+),(\d+),(\d+) row=(\d+)", timeout=10, since=since)
    v = [int(x) for x in m.groups()]
    return {"up": (v[0], v[1]), "newfolder": (v[2], v[3]), "ok": (v[4], v[5]), "cancel": (v[6], v[7]),
            "name": (v[8], v[9]), "list": (v[10], v[11], v[12], v[13]), "row": v[14]}


# ---------------------------------------------------------------------------
def s_golden_text(d):
    since = d.g.mark()
    d.g.key("ctrl", "4")
    d.wait_focus("Settings", since)
    d.g.wait_for(r"@@WIN_OPEN id=\d+ app=Settings", timeout=10, since=since)
    w = window(d, "Settings")
    r = golden(d, "settings-appearance", (w["x"], w["y"], w["w"], w["h"]))
    d.g.key("ctrl", "w")
    return r


def s_resize(d):
    set_mode(d, 1920, 1200)
    try:
        return resize_steps(d)
    finally:
        try:
            close_notes_discard(d)
        except GuestError:
            pass
        set_mode(d, 1024, 768)


def resize_steps(d):
    d.g.key("ctrl", "m")  # Terminal out of the way
    time.sleep(0.5)
    park_cursor(d)
    time.sleep(0.6)
    empty = shot(d, "resize-empty")
    open_notes_fresh(d)
    d.g.type_text("Resize me.\nLine two.")
    results = []
    for (cw, ch) in ((200, 150), (1600, 1000), (640, 400)):
        w = window(d, "Notes")
        gx, gy = w["x"] + w["w"] - 4, w["y"] + w["h"] - 4
        tx, ty = w["x"] + cw - 1, w["y"] + ch + TITLE_H - 1
        since = d.g.mark()
        d.drag(gx, gy, tx, ty)
        m = d.g.wait_for(r"@@WIN id=\d+ app=Notes x=-?\d+ y=-?\d+ w=(\d+) h=(\d+) state=normal", timeout=10,
                         since=since)
        got = (int(m.group(1)), int(m.group(2)) - TITLE_H)
        if got != (cw, ch):
            raise GuestError(f"resize to content {cw}x{ch} gave {got}")
        wait_ack(d, "Notes", cw, ch, since)
        w = window(d, "Notes")
        if (cw, ch) != (640, 400):
            results.append(golden(d, f"notes-{cw}x{ch}", (w["x"], w["y"], w["w"], w["h"])))
    # Leftovers: outside the window (and its shadow), the screen must equal
    # the empty desktop (the clock is masked).
    park_cursor(d)
    time.sleep(0.6)
    after = shot(d, "resize-after")
    w = window(d, "Notes")
    margin = 24
    # Masked: the window and its shadow, the menu bar (it names the focused
    # app), the dock (running-app dots) and the parked cursor.
    dx, dy = d.dock["Terminal"]
    mask = [(w["x"] - margin, w["y"] - margin, w["w"] + 2 * margin, w["h"] + 2 * margin),
            (0, 0, d.screen[0], 26), (dx - 60, dy - 60, 400, 120), (0, d.screen[1] - 30, 30, 30)]
    bad, total, worst = imgdiff.diff(after, empty, TOL, mask)
    if bad:
        raise GuestError(f"{bad} pixels outside the window differ from the empty desktop (max delta {worst})")
    return "; ".join(results) + "; no leftovers outside the window after 200x150 -> 1600x1000 -> 640x400"


def s_golden_dialogs(d):
    open_notes_fresh(d)
    d.g.type_text("dialog golden")
    w = window(d, "Notes")
    since = d.g.mark()
    d.g.key("ctrl", "s")
    d.g.wait_for(r"@@DIALOG kind=save dir=/home/notes", timeout=10, since=since)
    d.g.wait_for(r"@@DIALOG_DIR path=/home/notes entries=\d+", timeout=10, since=since)
    r1 = golden(d, "dialog-save", (w["x"], w["y"], w["w"], w["h"]))
    d.g.key("esc")
    d.g.wait_for(r"@@DIALOG_DONE result=cancel", timeout=10, since=since)
    since = d.g.mark()
    d.g.key("ctrl", "o")
    d.g.wait_for(r"@@NOTES_DIALOG unsaved", timeout=10, since=since)
    d.g.key("d")
    d.g.wait_for(r"@@DIALOG kind=open dir=/home/notes", timeout=10, since=since)
    r2 = golden(d, "dialog-open", (w["x"], w["y"], w["w"], w["h"]))
    d.g.key("esc")
    d.g.wait_for(r"@@DIALOG_DONE result=cancel", timeout=10, since=since)
    close_notes_discard(d)
    return f"{r1}; {r2}"


ROUNDTRIP = "Saved through the dialog.\nSecond line: 1234567890.\n"


def s_save_dialog_roundtrip(d):
    open_notes_fresh(d)
    d.g.type_text(ROUNDTRIP)
    w = window(d, "Notes")
    cx, cy = w["x"], w["y"] + TITLE_H
    since = d.g.mark()
    d.g.key("ctrl", "s")
    lay = layout(d, since)
    # New Folder "docs"
    since = d.g.mark()
    d.click(cx + lay["newfolder"][0], cy + lay["newfolder"][1])
    time.sleep(0.3)
    d.g.type_text("docs\n")
    d.g.wait_for(r"@@DIALOG_MKDIR path=/home/notes/docs result=0", timeout=10, since=since)
    d.g.wait_for(r"@@DIALOG_DIR path=/home/notes/docs entries=0", timeout=10, since=since)
    # Name field: select everything, type the name, Enter.
    d.click(cx + lay["name"][0], cy + lay["name"][1])
    d.g.key("ctrl", "a")
    since = d.g.mark()
    d.g.type_text("roundtrip.txt\n")
    m = d.g.wait_for(r"@@NOTES_SAVE path=(\S+) result=(-?\d+) len=(\d+) crc=([0-9a-f]+)", timeout=10, since=since)
    if m.group(1) != "/home/notes/docs/roundtrip.txt" or m.group(2) != "0":
        raise GuestError(f"save went wrong: {m.group(0)}")
    if int(m.group(3)) != len(ROUNDTRIP) or m.group(4) != crc(ROUNDTRIP):
        raise GuestError(f"saved bytes differ: {m.group(0)}")
    # Saving again over the same name asks for confirmation.
    since = d.g.mark()
    d.g.key("ctrl", "shift", "s")
    d.g.wait_for(r"@@DIALOG kind=save dir=/home/notes/docs", timeout=10, since=since)
    d.g.key("ret")
    d.g.wait_for(r"@@DIALOG_CONFIRM overwrite path=/home/notes/docs/roundtrip.txt", timeout=10, since=since)
    d.g.key("ret")
    d.g.wait_for(r"@@NOTES_SAVE path=/home/notes/docs/roundtrip.txt result=0", timeout=10, since=since)
    # New note, then reopen through the open dialog: folder, then file.
    since = d.g.mark()
    d.g.key("ctrl", "n")
    d.g.wait_for(r"@@NOTES_STATUS err=0 msg=New note", timeout=10, since=since)
    since = d.g.mark()
    d.g.key("ctrl", "o")
    d.g.wait_for(r"@@DIALOG kind=open dir=/home/notes", timeout=10, since=since)
    d.g.wait_for(r"@@DIALOG_DIR path=/home/notes entries=\d+", timeout=10, since=since)
    d.g.key("ret")  # "docs" is first (folders before files)
    d.g.wait_for(r"@@DIALOG_DIR path=/home/notes/docs entries=1", timeout=10, since=since)
    d.g.key("ret")
    m = d.g.wait_for(r"@@NOTES_LOAD path=(\S+) result=0 len=(\d+) crc=([0-9a-f]+)", timeout=10, since=since)
    if m.group(1) != "/home/notes/docs/roundtrip.txt" or int(m.group(2)) != len(ROUNDTRIP) or m.group(3) != crc(ROUNDTRIP):
        raise GuestError(f"reopened file differs: {m.group(0)}")
    close_notes_discard(d)
    return f"saved {len(ROUNDTRIP)} B into a new folder, overwrite confirmed, reopened identical (crc {crc(ROUNDTRIP)})"


CLIP = "copied across processes"


def s_clipboard_after_exit(d):
    open_notes_fresh(d)
    d.g.type_text(CLIP)
    d.g.key("ctrl", "a")
    since = d.g.mark()
    d.g.key("ctrl", "c")
    m = d.g.wait_for(r"@@CLIP_SET len=(\d+) crc=([0-9a-f]+) from=Notes", timeout=10, since=since)
    if (int(m.group(1)), m.group(2)) != (len(CLIP), crc(CLIP)):
        raise GuestError(f"copied {m.group(0)}")
    pid = re.findall(r"@@CLIENT app=Notes pid=(\d+)", d.g.serial())[-1]
    since = d.g.mark()
    close_notes_discard(d)
    d.g.wait_for(rf"@@PROC_EXIT pid={pid} name=notes status=0", timeout=10, since=since)
    term_focus(d)
    d.g.type_text("echo [")
    since = d.g.mark()
    d.g.key("ctrl", "v")
    d.g.wait_for(rf"@@TERM_PASTE added={len(CLIP)} ", timeout=10, since=since)
    d.g.type_text("]\n")
    d.g.wait_for(rf"@@TERM \[{re.escape(CLIP)}\]\s*$", timeout=10, since=since)
    return f"Notes (pid {pid}) exited; the Terminal pasted {len(CLIP)} characters from the server's copy"


def s_dead_keys_caps(d):
    term_focus(d)
    since = d.g.mark()
    term(d, "keymap us-intl")
    d.g.wait_for(r"@@KEYMAP name=US-International file=us-intl", timeout=10, since=since)
    since = d.g.mark()
    d.g.type_text("echo caf")
    d.g.key("apostrophe")
    d.g.key("e")
    d.g.wait_for(r"@@DEADKEY dead=dead_acute base=101 out=U\+00E9", timeout=10, since=since)
    d.g.type_text(" ")
    d.g.key("shift", "apostrophe")
    d.g.key("u")
    d.g.wait_for(r"@@DEADKEY dead=dead_diaeresis base=117 out=U\+00FC", timeout=10, since=since)
    d.g.type_text("ber\n")
    d.g.wait_for("@@TERM café über\\s*$", timeout=10, since=since)
    # Caps Lock: the LED follows, letters too.
    d.g.type_text("echo ")
    since = d.g.mark()
    d.g.key("caps_lock")
    d.g.wait_for(r"@@KBD_LEDS caps=1 ", timeout=10, since=since)
    d.g.key("a")
    d.g.key("b")
    since2 = d.g.mark()
    d.g.key("caps_lock")
    d.g.wait_for(r"@@KBD_LEDS caps=0 ", timeout=10, since=since2)
    d.g.type_text("c\n")
    d.g.wait_for(r"@@TERM ABc\s*$", timeout=10, since=since)
    since = d.g.mark()
    term(d, "keymap us")
    d.g.wait_for(r"@@KEYMAP name=US file=us", timeout=10, since=since)
    return "us-intl: ' e -> é, \" u -> ü reached the shell; Caps Lock LED on/off with ABc"


def s_wheel(d):
    term_focus(d)
    term(d, "ls /system/bin", r"@@TERM wc ")
    w = window(d, "Terminal")
    d.move_to(w["x"] + w["w"] // 2, w["y"] + TITLE_H + 60)
    since = d.g.mark()
    d.g.mouse_wheel(2)
    d.g.wait_for(r"@@WHEEL delta=1 win=\d+", timeout=10, since=since)
    m = d.g.wait_for(r"@@TERM_SCROLL wheel=1 scroll=(\d+)", timeout=10, since=since)
    since = d.g.mark()
    d.g.mouse_wheel(-2)
    d.g.wait_for(r"@@TERM_SCROLL wheel=-1 scroll=0", timeout=10, since=since)
    return f"wheel up scrolled the Terminal back ({m.group(1)} lines), wheel down returned"


def s_relayout(d):
    # Four windows: Terminal, Files, Notes (maximized), Settings.
    term_focus(d)
    for app in ("Files", "Notes"):
        since = d.g.mark()
        d.dock_click(app)
        d.wait_focus(app, since)
    since = d.g.mark()
    d.click(*d.control("Notes", "max"))
    d.g.wait_for(r"@@WIN id=\d+ app=Notes .* state=maximized", timeout=10, since=since)
    since = d.g.mark()
    d.dock_click("Settings")
    d.wait_focus("Settings", since)
    w = window(d, "Settings")
    # Settings > Display, first mode (800x600), Apply.
    tab_x = w["x"] + (w["w"] - 500) // 2 + 1 * 100 + 50
    since = d.g.mark()
    d.click(tab_x, w["y"] + TITLE_H + 25)
    d.g.wait_for(r"@@SETTINGS tab=Display", timeout=10, since=since)
    d.g.wait_for(r"@@SETTINGS_STATE w=1024 h=768 ", timeout=10, since=since)
    d.click(w["x"] + 24 + 60, w["y"] + TITLE_H + 108 + 1 + 12)
    since = d.g.mark()
    d.click(w["x"] + 284 + 60, w["y"] + TITLE_H + 108 + 14)
    d.g.wait_for(r"@@SETTINGS_MODE w=800 h=600", timeout=10, since=since)
    d.g.wait_for(r"@@SCREEN w=800 h=600", timeout=20, since=since)
    time.sleep(1.0)
    text = d.g.serial()[since:]
    wins = {}
    for m in re.finditer(r"@@WIN id=(\d+) app=(\w+) x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+) state=(\w+)", text):
        wins[m.group(2)] = tuple(int(m.group(i)) for i in range(3, 7)) + (m.group(7),)
    need = {"Terminal", "Files", "Notes", "Settings"}
    if not need <= set(wins):
        raise GuestError(f"not every window was re-laid out: {sorted(wins)}")
    dock_top = int(re.findall(r"@@DOCK_ITEM app=Terminal x=\d+ y=(\d+)", text)[-1]) - 10
    for app, (x, y, ww, hh, st) in wins.items():
        if x < 0 or y < 26 or x + ww > 800 or y + hh > dock_top:
            raise GuestError(f"{app} at {(x, y, ww, hh)} does not fit 800x600 (work area ends at {dock_top})")
        if app == "Notes" and (st != "maximized" or ww < 700):
            raise GuestError(f"maximized Notes not refitted: {(x, y, ww, hh, st)}")
    # Every resized window acknowledged its new size with a new buffer.
    for app in ("Notes", "Settings"):
        x, y, ww, hh, st = wins[app]
        if not re.search(rf"@@WIN_ACK id=\d+ app={app} serial=\d+ w={ww} h={hh - TITLE_H} match=1",
                         d.g.serial()[since:]):
            d.g.wait_for(rf"@@WIN_ACK id=\d+ app={app} serial=\d+ w={ww} h={hh - TITLE_H} match=1", timeout=10,
                         since=since)
    refresh(d)
    shot(d, "relayout-800x600")
    # Back to 1024x768 and close everything.
    set_mode(d, 1024, 768)
    for _ in range(4):
        since = d.g.mark()
        d.g.key("ctrl", "w")
        m = d.g.wait_for(r"@@(NOTES_DIALOG unsaved|WIN_CLOSE id=\d+)", timeout=10, since=since)
        if "unsaved" in m.group(0):
            d.g.key("d")
            d.g.wait_for(r"@@WIN_CLOSE id=\d+", timeout=10, since=since)
    return "800x600: 4 windows fit the new work area, maximized Notes refitted, resized ones redrew"


def s_kill_app(d):
    open_notes_fresh(d)
    pid = re.findall(r"@@CLIENT app=Notes pid=(\d+)", d.g.serial())[-1]
    term_focus(d)
    since = d.g.mark()
    term(d, f"kill {pid}")
    d.g.wait_for(rf"@@PROC_EXIT pid={pid} name=notes status=9", timeout=10, since=since)
    d.g.wait_for(r"@@CLIENT_GONE app=Notes", timeout=10, since=since)
    d.g.wait_for(r"@@WIN_CLOSE id=\d+", timeout=10, since=since)
    term(d, "echo still-alive", r"@@TERM still-alive\s*$")
    since = d.g.mark()
    d.dock_click("Notes")
    d.wait_focus("Notes", since)
    d.g.key("ctrl", "w")
    return f"Notes (pid {pid}) killed: its window went away, the Terminal and the server kept working"


def s_kill_display(d):
    text = d.g.serial()
    old = re.findall(r"@@DISPLAY_READY w=\d+ h=\d+ pid=(\d+)", text)[-1]
    term_focus(d)
    since = d.g.mark()
    d.g.type_text(f"kill {old}\n")
    d.g.wait_for(rf"@@PROC_EXIT pid={old} name=display status=9", timeout=10, since=since)
    d.g.wait_for(r"@@DISPLAY_EXIT status=9", timeout=10, since=since)
    m = d.g.wait_for(r"@@DISPLAY_READY w=(\d+) h=(\d+) pid=(\d+)", timeout=30, since=since)
    if m.group(3) == old:
        raise GuestError("display server was not restarted")
    d.g.wait_for(r"@@FOCUS id=\d+ app=Welcome", timeout=30, since=since)
    refresh(d)
    d.cursor = (int(m.group(1)) // 2, int(m.group(2)) // 2)
    d.g.key("ret")
    d.g.wait_for(r"@@WIN_CLOSE id=\d+", timeout=10, since=since)
    since2 = d.g.mark()
    d.g.key("ctrl", "1")
    d.wait_focus("Terminal", since2)
    term(d, "echo session-restarted", r"@@TERM session-restarted\s*$")
    if "AURORA_PANIC" in d.g.serial():
        raise GuestError("kernel panic")
    shot(d, "after-display-restart")
    return f"display (pid {old}) killed; init restarted it as pid {m.group(3)}; a new Terminal works"


SCENARIOS = [
    ("golden-text", s_golden_text),
    ("golden-dialogs", s_golden_dialogs),
    ("save-dialog-roundtrip", s_save_dialog_roundtrip),
    ("clipboard-after-exit", s_clipboard_after_exit),
    ("dead-keys-caps", s_dead_keys_caps),
    ("wheel", s_wheel),
    ("resize-200-1600", s_resize),
    ("resolution-relayout", s_relayout),
    ("kill-app", s_kill_app),
    ("kill-display", s_kill_display),  # last: restarts the session
]


def main():
    global UPDATE
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--update-goldens", action="store_true")
    ap.add_argument("--memory", default="512M")
    ap.add_argument("--smp", type=int, default=None)
    args = ap.parse_args()
    UPDATE = args.update_goldens
    if not IMG.exists():
        sys.exit("error: build/test/aurora.img missing; run 'python tools/build.py build --variant test'")
    SHOTS.mkdir(parents=True, exist_ok=True)
    results = []
    g = Guest(IMG, memory=args.memory, name="desktop", keep_dir=OUT / "desktop-guest", smp=args.smp)
    try:
        g.start()
        d = Desktop(g)
        d.boot()
        g.wait_for(r"@@FOCUS id=1 app=Welcome", timeout=30)
        g.key("ret")
        g.wait_for(r"@@WIN_CLOSE id=1", timeout=10)
        for name, fn in SCENARIOS:
            if args.only and name not in args.only:
                continue
            t0 = time.time()
            try:
                detail = fn(d)
                results.append({"test": name, "result": "pass", "detail": detail, "seconds": round(time.time() - t0, 1)})
                print(f"PASS  {name}: {detail}")
            except GuestError as e:
                msg = str(e)
                results.append({"test": name, "result": "fail", "detail": msg.splitlines()[0],
                                "seconds": round(time.time() - t0, 1)})
                print(f"FAIL  {name}: {msg[:1500]}")
                try:
                    g.screendump(SHOTS / f"fail-{name}.png")
                except Exception:
                    pass
                if not g.alive():
                    break
                # Leave a clean desktop for the next scenario.
                try:
                    d.close_all()
                except Exception:
                    pass
            if "LOCKDEP" in g.serial():
                print("FAIL  lock validator report in the serial log")
                results.append({"test": "lockdep", "result": "fail", "detail": "LOCKDEP report", "seconds": 0})
                break
    finally:
        g.stop()
    (OUT / "desktop.json").write_text(json.dumps(results, indent=2))
    failed = [r for r in results if r["result"] != "pass"]
    print(f"desktop: {len(results) - len(failed)}/{len(results)} passed")
    sys.exit(1 if failed or not results else 0)


if __name__ == "__main__":
    main()
