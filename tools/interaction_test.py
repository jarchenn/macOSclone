#!/usr/bin/env python3
"""Guest interaction tests: real PS/2 input injected through QMP into the
`test` build, with assertions on structured serial markers ("@@...") that the
kernel emits only in that build, plus QEMU screenshots.

Usage: interaction_test.py [--only NAME ...] [--soak SECONDS]
Results: build/test-results/interaction.json, screenshots in the same dir.
"""
import argparse
import json
import re
import sys
import time
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.stdout.reconfigure(errors="replace")  # guest output may contain any bytes
import aurora_env as env  # noqa: E402
from qemu_harness import Guest, GuestError  # noqa: E402

OUT = env.BUILD / "test-results"
IMG = env.BUILD / "test" / "aurora.img"
TITLE_H = 30
TOOLBAR_H, HEADER_H, ROW_H, SIDEBAR_W = 44, 26, 26, 150


class Desktop:
    def __init__(self, guest):
        self.g = guest
        self.cursor = None
        self.dock = {}
        self.screen = None

    def boot(self):
        self.g.wait_for(r"AURORA_DESKTOP_READY", timeout=180)
        text = self.g.serial()
        m = re.search(r"@@SCREEN w=(\d+) h=(\d+)", text)
        self.screen = (int(m.group(1)), int(m.group(2)))
        for m in re.finditer(r"@@DOCK_ITEM app=(\w+) x=(\d+) y=(\d+) w=(\d+) h=(\d+)", text):
            self.dock[m.group(1)] = (int(m.group(2)) + int(m.group(4)) // 2, int(m.group(3)) + int(m.group(5)) // 2)
        c = re.findall(r"@@CURSOR x=(\d+) y=(\d+)", text)
        self.cursor = (int(c[-1][0]), int(c[-1][1]))
        self.g.mark()

    # -- geometry from markers ---------------------------------------------
    def windows(self):
        wins = {}
        for m in re.finditer(r"@@WIN id=(\d+) app=(\w+) x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+) state=(\w+)", self.g.serial()):
            wins[int(m.group(1))] = dict(id=int(m.group(1)), app=m.group(2), x=int(m.group(3)), y=int(m.group(4)),
                                         w=int(m.group(5)), h=int(m.group(6)), state=m.group(7))
        for m in re.finditer(r"@@WIN_CLOSE id=(\d+)", self.g.serial()):
            wins.pop(int(m.group(1)), None)
        return wins

    def window(self, app):
        ws = [w for w in self.windows().values() if w["app"] == app]
        if not ws:
            raise GuestError(f"no open window for {app}")
        return ws[-1]

    def focused(self):
        f = re.findall(r"@@FOCUS id=(\d+) app=(\w+)", self.g.serial())
        return f[-1][1] if f else None

    # -- input ---------------------------------------------------------------
    def move_to(self, x, y):
        x = max(0, min(self.screen[0] - 1, x))
        y = max(0, min(self.screen[1] - 1, y))
        for _ in range(3):
            dx, dy = x - self.cursor[0], y - self.cursor[1]
            if dx == 0 and dy == 0:
                return
            since = self.g.mark()
            self.g.mouse_move(dx, dy)
            m = self.g.wait_for(rf"@@CURSOR x={x} y={y}\b", timeout=15, since=since)
            self.cursor = (x, y)
            return

    def click(self, x, y, double=False):
        self.move_to(x, y)
        for i in range(2 if double else 1):
            since = self.g.mark()
            self.g.mouse_button(True)
            self.g.wait_for(rf"@@CLICK x={x} y={y}", timeout=10, since=since)
            self.g.mouse_button(False)
            time.sleep(0.05 if double else 0.15)

    def drag(self, x0, y0, x1, y1):
        self.move_to(x0, y0)
        self.g.mouse_button(True)
        self.g.wait_for(rf"@@CLICK x={x0} y={y0}", timeout=10)
        steps = 6
        for i in range(1, steps + 1):
            self.move_to(x0 + (x1 - x0) * i // steps, y0 + (y1 - y0) * i // steps)
        self.g.mouse_button(False)
        time.sleep(0.2)

    def dock_click(self, app):
        since = self.g.mark()
        self.click(*self.dock[app])
        self.g.wait_for(rf"@@DOCK_CLICK app={app}", timeout=10, since=since)

    def title_point(self, app, dx=None):
        w = self.window(app)
        return (w["x"] + (dx if dx is not None else w["w"] // 2), w["y"] + TITLE_H // 2)

    def control(self, app, which):
        w = self.window(app)
        off = {"close": 20, "min": 40, "max": 60}[which]
        return (w["x"] + off, w["y"] + 15)

    def content_point(self, app, cx, cy):
        w = self.window(app)
        return (w["x"] + cx, w["y"] + TITLE_H + cy)

    def terminal(self, line, expect=None, timeout=15):
        since = self.g.mark()
        self.g.type_text(line + "\n")
        self.g.wait_for(r"@@TERM aurora:\S*\$ " + re.escape(line) + r"\s*$", timeout=timeout, since=since)
        if expect:
            return self.g.wait_for(expect, timeout=timeout, since=since)
        return None

    def wait_focus(self, app, since=None, timeout=15):
        """Wait until the most recent FOCUS marker names `app` (it may
        already be focused, in which case no new marker is emitted)."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.focused() == app:
                time.sleep(0.15)  # let the event that caused it finish
                if self.focused() == app:
                    return
            if "AURORA_PANIC" in self.g.serial() or not self.g.alive():
                raise GuestError("guest died")
            time.sleep(0.05)
        raise GuestError(f"focus is {self.focused()}, expected {app}")

    def close_all(self):
        """Close every window with Ctrl+W (none of them have unsaved data)."""
        for _ in range(20):
            wins = [w for w in self.windows().values() if w["state"] != "minimized"]
            if not wins:
                return
            since = self.g.mark()
            self.g.key("ctrl", "w")
            self.g.wait_for(r"@@WIN_CLOSE id=\d+", timeout=10, since=since)
        raise GuestError("could not close all windows")

    def open_app(self, app):
        since = self.g.mark()
        self.dock_click(app)
        self.wait_focus(app, since)


def crc(s: str) -> str:
    return f"{zlib.crc32(s.encode()) & 0xFFFFFFFF:08x}"


# ---------------------------------------------------------------------------
# Scenarios. Each raises on failure; returns a short detail string.

def s_launch_each_app(d):
    for app in ("Terminal", "Files", "Notes", "Settings"):
        since = d.g.mark()
        d.dock_click(app)
        d.g.wait_for(rf"@@WIN_OPEN id=\d+ app={app}\b", timeout=15, since=since)
        d.wait_focus(app, since)
    d.g.screendump(OUT / "apps-open.png")
    # clicking a dock icon of a running app focuses it (no second window)
    since = d.g.mark()
    d.dock_click("Terminal")
    d.wait_focus("Terminal", since)
    if len([w for w in d.windows().values() if w["app"] == "Terminal"]) != 1:
        raise GuestError("dock click opened a duplicate Terminal")
    return "4 apps launched and focused from the dock"


def s_drag_overlap_focus(d):
    d.close_all()
    d.open_app("Terminal")
    d.open_app("Files")
    term = d.window("Terminal")
    files = d.window("Files")
    # Focus the Terminal, then raise Files by clicking its title bar.
    d.click(term["x"] + 40, term["y"] + TITLE_H + 200)
    d.wait_focus("Terminal")
    # (a point of the Files title bar not covered by the now-raised Terminal)
    d.click(*d.title_point("Files", files["w"] - 30))
    d.wait_focus("Files")
    tx, ty = term["x"] + 200, term["y"] + 60
    start = d.title_point("Files", 120)
    d.drag(start[0], start[1], tx + 120, ty + TITLE_H // 2)
    nf = d.window("Files")
    if (nf["x"], nf["y"]) != (tx, ty):
        raise GuestError(f"Files window at {(nf['x'], nf['y'])}, expected {(tx, ty)}")
    # Click inside the overlap: the topmost window (Files) must receive it.
    since = d.g.mark()
    ox, oy = nf["x"] + 300, nf["y"] + TITLE_H + 150
    d.click(ox, oy)
    time.sleep(0.3)
    if d.focused() != "Files":
        raise GuestError(f"overlap click focused {d.focused()}")
    # Click a visible part of the Terminal (left of the Files window).
    since = d.g.mark()
    d.click(term["x"] + 40, term["y"] + TITLE_H + 200)
    d.wait_focus("Terminal", since)
    # Keyboard input now goes to the Terminal.
    d.terminal("echo focus-routed", r"@@TERM focus-routed\s*$")
    d.g.screendump(OUT / "overlap.png")
    # Title bar stays reachable: dragging far up stops at the menu bar.
    p = d.title_point("Files", 100)
    d.drag(p[0], p[1], p[0], 2)
    if d.window("Files")["y"] < 26:
        raise GuestError("window dragged above the menu bar")
    return f"Files dragged to {(tx, ty)}; top window got the click; keyboard followed focus"


def s_minimize_restore(d):
    since = d.g.mark()
    d.click(*d.title_point("Terminal", 200))
    d.wait_focus("Terminal", since)
    since = d.g.mark()
    d.click(*d.control("Terminal", "min"))
    d.g.wait_for(r"@@WIN id=\d+ app=Terminal .* state=minimized", timeout=10, since=since)
    if d.focused() == "Terminal":
        raise GuestError("minimized window kept focus")
    # Keys typed now must not reach the minimized Terminal.
    d.g.key("z")
    d.g.key("z")
    since = d.g.mark()
    d.dock_click("Terminal")
    d.g.wait_for(r"@@WIN id=\d+ app=Terminal .* state=normal", timeout=10, since=since)
    d.wait_focus("Terminal", since)
    d.terminal("echo restored", r"@@TERM restored\s*$")
    # Maximize and restore with the green control.
    since = d.g.mark()
    d.click(*d.control("Terminal", "max"))
    d.g.wait_for(r"@@WIN id=\d+ app=Terminal .* state=maximized", timeout=10, since=since)
    d.g.screendump(OUT / "maximized.png")
    since = d.g.mark()
    d.click(*d.control("Terminal", "max"))
    d.g.wait_for(r"@@WIN id=\d+ app=Terminal .* state=normal", timeout=10, since=since)
    return "minimize -> dock restore -> no leaked keys; maximize/restore ok"


NOTE1 = "Aurora note 1: spaces,  double  spaces.\nLine two - punctuation !?;:'\"()[]{}<>@#$%^&*_+=|\\/~`\n\nLast line."
NOTE2 = "Second note\n  indented line\nend."


def open_notes_fresh(d):
    since = d.g.mark()
    d.dock_click("Notes")
    d.wait_focus("Notes", since)
    # Ctrl+N starts an empty note (discarding is not needed: not dirty)
    since = d.g.mark()
    d.g.key("ctrl", "n")
    d.g.wait_for(r"@@NOTES_STATUS err=0 msg=New note", timeout=10, since=since)


def save_as(d, name):
    since = d.g.mark()
    d.g.key("ctrl", "s")
    d.g.wait_for(r"@@NOTES_DIALOG saveas", timeout=10, since=since)
    for _ in range(len("untitled.txt")):
        d.g.key("backspace")
    since = d.g.mark()
    d.g.type_text(name + "\n")
    return d.g.wait_for(r"@@NOTES_SAVE path=(\S+) result=(-?\d+) len=(\d+) crc=([0-9a-f]+)", timeout=15, since=since)


def files_open_note(d, name, index):
    since = d.g.mark()
    d.dock_click("Files")
    d.wait_focus("Files", since)
    # Sidebar "Notes" location
    since = d.g.mark()
    d.click(*d.content_point("Files", 60, TOOLBAR_H + 30 + 28 + 13))
    d.g.wait_for(r"@@FILES path=/home/notes entries=(\d+)", timeout=10, since=since)
    y = TOOLBAR_H + HEADER_H + index * ROW_H + ROW_H // 2
    since = d.g.mark()
    d.click(*d.content_point("Files", SIDEBAR_W + 80, y), double=True)
    d.g.wait_for(rf"@@FILES_OPEN path=/home/notes/{re.escape(name)}", timeout=10, since=since)
    return d.g.wait_for(r"@@NOTES_LOAD path=(\S+) result=0 len=(\d+) crc=([0-9a-f]+)", timeout=10, since=since)


def s_notes_roundtrip(d):
    details = []
    for text, name in ((NOTE1, "test-note.txt"), (NOTE2, "second.txt")):
        open_notes_fresh(d)
        d.g.type_text(text)
        m = save_as(d, name)
        if m.group(2) != "0":
            raise GuestError(f"save failed: {m.group(0)}")
        if m.group(1) != f"/home/notes/{name}" or int(m.group(3)) != len(text) or m.group(4) != crc(text):
            raise GuestError(f"saved bytes differ: {m.group(0)} expected len={len(text)} crc={crc(text)}")
        # Close Notes with the red control (no unsaved changes -> closes).
        since = d.g.mark()
        d.click(*d.control("Notes", "close"))
        d.g.wait_for(r"@@WIN_CLOSE id=\d+", timeout=10, since=since)
        # Reopen through Files; sorted listing: example.txt, second.txt, test-note.txt
        names = sorted(["example.txt", "test-note.txt"] + (["second.txt"] if name == "second.txt" else []))
        m = files_open_note(d, name, names.index(name))
        if int(m.group(2)) != len(text) or m.group(3) != crc(text):
            raise GuestError(f"reopened content differs: {m.group(0)}")
        if name == "test-note.txt":
            time.sleep(0.5)
            d.g.screendump(OUT / "note-reopened.png")
        # Same bytes through the Terminal
        since = d.g.mark()
        d.dock_click("Terminal")
        d.wait_focus("Terminal", since)
        m = d.terminal(f"cat /home/notes/{name}", r"@@CAT path=(\S+) len=(\d+) crc=([0-9a-f]+)")
        if int(m.group(2)) != len(text) or m.group(3) != crc(text):
            raise GuestError(f"cat content differs: {m.group(0)}")
        details.append(f"{name}: {len(text)} B crc {crc(text)}")
    return "; ".join(details) + " (Notes save == Files reopen == Terminal cat)"


def s_readonly_save(d):
    since = d.g.mark()
    d.dock_click("Terminal")
    d.wait_focus("Terminal", since)
    since = d.g.mark()
    d.terminal("open /system/about.txt")
    m = d.g.wait_for(r"@@NOTES_LOAD path=/system/about.txt result=0 len=(\d+)", timeout=10, since=since)
    orig = int(m.group(1))
    d.wait_focus("Notes", since)
    d.g.key("ctrl", "s")  # nothing changed yet: still refused (read-only)
    d.g.key("end")
    d.g.type_text("X")
    since = d.g.mark()
    d.g.key("ctrl", "s")
    m = d.g.wait_for(r"@@NOTES_SAVE path=/system/about.txt result=(-?\d+) len=(\d+)", timeout=10, since=since)
    if m.group(1) != "-5" or int(m.group(2)) != orig + 1:
        raise GuestError(f"expected EROFS (-5) with len {orig + 1}: {m.group(0)}")
    d.g.wait_for(r"@@NOTES_STATUS err=1 msg=Save failed: read-only file system", timeout=5, since=since)
    # The edit is still in the buffer: a second attempt still carries orig+1 bytes.
    since = d.g.mark()
    d.g.key("ctrl", "s")
    m = d.g.wait_for(r"@@NOTES_SAVE path=/system/about.txt result=-5 len=(\d+)", timeout=10, since=since)
    if int(m.group(1)) != orig + 1:
        raise GuestError("edit lost after failed save")
    d.g.screendump(OUT / "readonly-save.png")
    # Close with unsaved changes: Cancel keeps the window, Don't Save closes.
    since = d.g.mark()
    d.g.key("ctrl", "w")
    d.g.wait_for(r"@@NOTES_DIALOG unsaved", timeout=10, since=since)
    d.g.key("esc")
    time.sleep(0.3)
    if not [w for w in d.windows().values() if w["app"] == "Notes"]:
        raise GuestError("Cancel closed the window")
    since = d.g.mark()
    d.g.key("ctrl", "w")
    d.g.wait_for(r"@@NOTES_DIALOG unsaved", timeout=10, since=since)
    d.g.key("d")
    d.g.wait_for(r"@@WIN_CLOSE id=\d+", timeout=10, since=since)
    # /system file unchanged
    d.dock_click("Terminal")
    m = d.terminal("cat /system/about.txt", r"@@CAT path=/system/about.txt len=(\d+)")
    if int(m.group(1)) != orig:
        raise GuestError("read-only file changed")
    return f"EROFS reported, {orig + 1}-byte buffer kept, Cancel/Don't Save work, /system unchanged"


def s_unsaved_save_path(d):
    """Unsaved dialog 'Save' on a writable file saves then closes."""
    since = d.g.mark()
    d.terminal("open /home/notes/second.txt")
    d.g.wait_for(r"@@NOTES_LOAD path=/home/notes/second.txt result=0", timeout=10, since=since)
    d.g.key("ctrl", "end") if False else d.g.key("end")
    d.g.type_text("!")
    since = d.g.mark()
    d.g.key("ctrl", "w")
    d.g.wait_for(r"@@NOTES_DIALOG unsaved", timeout=10, since=since)
    d.g.key("ret")
    m = d.g.wait_for(r"@@NOTES_SAVE path=/home/notes/second.txt result=0 len=(\d+)", timeout=10, since=since)
    d.g.wait_for(r"@@WIN_CLOSE id=\d+", timeout=10, since=since)
    return f"saved {m.group(1)} B via dialog, then closed"


def s_theme_switch(d):
    for app in ("Files", "Settings"):
        since = d.g.mark()
        d.dock_click(app)
        d.wait_focus(app, since)
    since = d.g.mark()
    d.dock_click("Terminal")
    d.wait_focus("Terminal", since)
    since = d.g.mark()
    d.terminal("theme dark")
    d.g.wait_for(r"@@THEME dark", timeout=10, since=since)
    time.sleep(0.8)
    d.g.screendump(OUT / "theme-dark.png")
    # Switch back through Settings > Appearance > Light card
    since = d.g.mark()
    d.dock_click("Settings")
    d.wait_focus("Settings", since)
    s = d.window("Settings")
    tab_x = (s["w"] - 330) // 2 + 55
    d.click(*d.content_point("Settings", tab_x, 25))
    since = d.g.mark()
    d.click(*d.content_point("Settings", 24 + 90, 96 + 60))
    d.g.wait_for(r"@@THEME light", timeout=10, since=since)
    time.sleep(0.5)
    d.g.screendump(OUT / "theme-light.png")
    return "dark via Terminal, light via Settings, 3+ windows open"


def s_terminal_errors(d):
    since = d.g.mark()
    d.dock_click("Terminal")
    d.wait_focus("Terminal", since)
    d.terminal("frobnicate", r"@@TERM frobnicate: command not found")
    d.terminal("cat /home/does-not-exist.txt", r"@@TERM cat: /home/does-not-exist.txt: no such file or directory")
    d.terminal("write /system/hack.txt hello", r"@@TERM write: /system/hack.txt: read-only file system")
    d.terminal("touch /system/x", r"@@TERM touch: /system/x: read-only file system")
    d.terminal("cd /system/about.txt", r"@@TERM cd: /system/about.txt: not a directory")
    d.terminal('echo "unterminated', r"@@TERM error: unterminated quote")
    d.terminal("ls /home/notes", r"@@TERM test-note.txt")
    d.terminal("mem", r"@@TERM Kernel heap in use:")
    d.terminal("uptime", r"@@TERM up \d+:\d\d:\d\d")
    d.terminal('write /home/q.txt "two  spaces\\nline"', r"@@TERM wrote 16 bytes to /home/q.txt")
    d.terminal("cat /home/q.txt", r"@@CAT path=/home/q.txt len=16 crc=" + crc("two  spaces\nline"))
    return "invalid command, missing file, /system write, quoting all handled"


def s_limits(d):
    since = d.g.mark()
    d.dock_click("Terminal")
    d.wait_focus("Terminal", since)
    # Command-line limit: 200 characters accepted, the 201st refused.
    since = d.g.mark()
    d.g.type_text("echo " + "x" * 195)
    d.g.type_text("yz")
    d.g.wait_for(r"@@TERM_LIMIT len=200", timeout=30, since=since)
    since = d.g.mark()
    d.g.key("ret")
    # Output wraps at the terminal width (80 columns): 80 + 80 + 35 = 195.
    d.g.wait_for(r"@@TERM x{80}\s*\n(?:\r?\n)*@@TERM x{80}\s*\n(?:\r?\n)*@@TERM x{35}\s*$", timeout=10, since=since)
    # Rapid input: back-to-back bursts without per-key delays. QEMU's
    # emulated i8042 holds only 16 scancode bytes, so each QMP burst is kept
    # to 7 keys (14 bytes); the kernel must lose nothing on its side.
    from qemu_harness import char_keys
    burst = "echo rapidburst0123456789abcdefghijklmnopqrstuvwxyz"
    since = d.g.mark()
    for i in range(0, len(burst), 7):
        events = []
        for ch in burst[i:i + 7]:
            keys = char_keys(ch)
            for k in keys:
                events.append({"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": k}}})
            for k in reversed(keys):
                events.append({"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": k}}})
        d.g.command("input-send-event", events=events)
        time.sleep(0.02)
    time.sleep(0.5)
    d.g.key("ret")
    d.g.wait_for(r"@@TERM rapidburst0123456789abcdefghijklmnopqrstuvwxyz\s*$", timeout=10, since=since)
    m = d.terminal("mem", r"@@TERM Input drops: +kbd (\d+), mouse (\d+), events (\d+)")
    if m.group(1) != "0" or m.group(3) != "0":
        raise GuestError(f"kernel dropped input: {m.group(0)}")
    # Notes capacity: a 16384-byte file opens; one more character is refused.
    d.terminal("testfill /home/full.txt 16384", r"@@TERM testfill: /home/full.txt 16384 bytes: success")
    since = d.g.mark()
    d.terminal("open /home/full.txt")
    d.g.wait_for(r"@@NOTES_LOAD path=/home/full.txt result=0 len=16384", timeout=10, since=since)
    since = d.g.mark()
    d.g.type_text("Q")
    d.g.wait_for(r"@@NOTES_FULL len=16384", timeout=10, since=since)
    d.g.key("ctrl", "s")  # not dirty -> saves identical content
    d.g.key("ctrl", "w")
    time.sleep(0.5)
    # Larger than Notes' buffer: open is refused cleanly.
    d.dock_click("Terminal")
    d.terminal("testfill /home/big.txt 20000", r"@@TERM testfill: /home/big.txt 20000 bytes: success")
    since = d.g.mark()
    d.terminal("open /home/big.txt")
    d.g.wait_for(r"@@NOTES_LOAD path=/home/big.txt result=-12", timeout=10, since=since)
    # RAM file size limit (64 KiB).
    d.dock_click("Terminal")
    d.terminal("testfill /home/huge.txt 65537", r"@@TERM testfill: /home/huge.txt 65537 bytes: file too large")
    d.g.key("ctrl", "w")
    return ("200-char line limit, 52-key burst intact with 0 kernel-side drops, "
            "16 KiB note cap, 20000 B file refused by Notes, 64 KiB file cap")


def s_edges(d):
    w, h = d.screen
    d.move_to(0, 0)
    d.move_to(w - 1, h - 1)
    d.move_to(0, h - 1)
    d.move_to(w - 1, 0)
    # Push far beyond the edges: the cursor must clamp.
    since = d.g.mark()
    d.g.mouse_move(-3000, 3000)
    d.g.wait_for(rf"@@CURSOR x=0 y={h - 1}", timeout=10, since=since)
    d.cursor = (0, h - 1)
    since = d.g.mark()
    d.g.mouse_move(5000, -5000)
    d.g.wait_for(rf"@@CURSOR x={w - 1} y=0", timeout=10, since=since)
    d.cursor = (w - 1, 0)
    return "cursor clamped at all four edges"


def s_open_close_leak(d):
    d.close_all()
    samples = []
    for i in range(12):
        for key in ("1", "2", "4"):
            since = d.g.mark()
            d.g.key("ctrl", key)
            d.g.wait_for(r"@@WIN_OPEN id=\d+", timeout=10, since=since)
        for _ in range(3):
            since = d.g.mark()
            d.g.key("ctrl", "w")
            m = d.g.wait_for(r"@@MEM why=close heap_used=(\d+) allocs=(\d+) mapped=(\d+)", timeout=10, since=since)
        samples.append((int(m.group(1)), int(m.group(2)), int(m.group(3))))
    first, last = samples[1], samples[-1]
    if last[0] > first[0] or last[1] > first[1]:
        raise GuestError(f"heap grew across open/close cycles: {first} -> {last}")
    if last[2] > first[2] + 4 * 1024 * 1024:
        raise GuestError(f"mapped heap kept growing: {first[2]} -> {last[2]}")
    return f"12 cycles x 3 windows; heap_used {first[0]} -> {last[0]}, allocs {first[1]} -> {last[1]}"


def s_menus(d):
    since = d.g.mark()
    d.dock_click("Terminal")
    d.wait_focus("Terminal", since)
    text = d.g.serial()
    m = re.search(r"@@MENUBAR aurora_x=(\d+) app_x=(\d+)", text)
    since = d.g.mark()
    d.click(int(m.group(1)), 12)
    mm = d.g.wait_for(r"@@MENU open=aurora x=(\d+) y=(\d+) w=(\d+) h=(\d+)", timeout=10, since=since)
    d.g.screendump(OUT / "menu.png")
    # first item "About Aurora" -> opens Settings on the About tab
    mx, my = int(mm.group(1)), int(mm.group(2))
    since = d.g.mark()
    d.click(mx + 40, my + 6 + 12)
    d.wait_focus("Settings", since)
    time.sleep(0.5)
    d.g.screendump(OUT / "about.png")
    d.g.key("ctrl", "w")
    return "Aurora menu opens and 'About Aurora' works"


def s_reboot(d):
    since = d.g.mark()
    d.dock_click("Terminal")
    d.wait_focus("Terminal", since)
    d.terminal("write /home/notes/volatile.txt session-only", r"@@TERM wrote")
    since = d.g.mark()
    d.g.type_text("reboot\n")
    d.g.wait_for(r"@@REBOOT", timeout=10, since=since)
    d.g.wait_for(r"AURORA_MILESTONE serial", timeout=120, since=since)
    d.g.wait_for(r"AURORA_DESKTOP_READY", timeout=180, since=since)
    d.boot_after = d.g.mark()
    # Fresh session: cursor is back at the centre.
    d.cursor = (d.screen[0] // 2, d.screen[1] // 2)
    d.g.key("ctrl", "1")
    d.g.wait_for(r"@@WIN_OPEN id=\d+ app=Terminal", timeout=15, since=d.boot_after)
    time.sleep(0.5)
    d.terminal("cat /home/notes/volatile.txt", r"@@TERM cat: /home/notes/volatile.txt: no such file or directory")
    d.terminal("cat /home/notes/example.txt", r"@@CAT path=/home/notes/example.txt len=\d+")
    return "reset via 8042; RAM file gone after restart, seeded example restored"


def s_soak(d, seconds):
    import random
    rnd = random.Random(1234)
    end = time.time() + seconds
    n = 0
    since = d.g.mark()
    d.g.key("ctrl", "1")
    while time.time() < end:
        r = rnd.random()
        if r < 0.4:
            d.g.mouse_move(rnd.randint(-80, 80), rnd.randint(-80, 80))
        elif r < 0.7:
            d.g.type_text(rnd.choice(["ls\n", "help\n", "mem\n", "echo soak\n", "uptime\n", "abc"]))
        elif r < 0.8:
            d.g.key("ctrl", rnd.choice(["1", "2", "3", "4"]))
        elif r < 0.85:
            d.g.key("ctrl", "tab")
        else:
            time.sleep(1.0)  # idle stretches exercise sti;hlt
        n += 1
        if "AURORA_PANIC" in d.g.serial() or not d.g.alive():
            raise GuestError("guest died during soak")
    text = d.g.serial()
    if text.count("AURORA_MILESTONE serial") != 1:
        raise GuestError("unexpected reset during soak")
    d.g.screendump(OUT / "soak-end.png")
    return f"{seconds}s, {n} random actions, no panic/reset"


# ---- 0.2: user processes ----------------------------------------------------

def term_focus(d):
    since = d.g.mark()
    d.dock_click("Terminal")
    d.wait_focus("Terminal", since)


def s_user_programs(d):
    term_focus(d)
    d.terminal("hello alpha beta", r"@@TERM Hello from user space! pid=\d+ argc=3 \[alpha\] \[beta\]")
    d.g.wait_for(r"@@PROC_EXIT pid=\d+ name=hello status=0", timeout=10)
    d.terminal("badptr", r"@@TERM badptr: 10/10 checks passed")
    d.g.wait_for(r"@@PROC_EXIT pid=\d+ name=badptr status=0", timeout=10)
    since = d.g.mark()
    d.terminal("crash")
    # 0.6: a fault is a signal; the wait status is the signal number.
    d.g.wait_for(r"@@PROC_EXIT pid=\d+ name=crash status=11 reason=segmentation fault: write unmapped at 0 ", timeout=10, since=since)
    for mode, sig, pat in (("kernel", 11, r"read denied at ffffffff80000000"), ("stack", 11, r"write unmapped at 7fff"),
                           ("ud", 4, r"invalid opcode"), ("gp", 11, r"general protection fault")):
        since = d.g.mark()
        d.terminal(f"crash {mode}")
        d.g.wait_for(rf"@@PROC_EXIT pid=\d+ name=crash status={sig} reason=.*" + pat, timeout=10, since=since)
    # the kernel and desktop are still fine
    d.terminal("uptime", r"@@TERM up \d+:\d\d:\d\d")
    d.terminal("ls /system/bin", r"@@TERM spin ")
    return "hello/badptr ran; 5 kinds of faults each killed only the faulting process"


def s_memory_isolation(d):
    term_focus(d)
    since = d.g.mark()
    d.terminal("memtest write SECRET-42 4000 &")
    m = d.g.wait_for(r"@@TERM memtest: pid (\d+) wrote SECRET-42 at (0x[0-9a-f]+)", timeout=10, since=since)
    writer, addr = m.group(1), m.group(2)
    m2 = d.terminal("memtest read", r"@@TERM memtest: pid (\d+) reads (\S+) at (0x[0-9a-f]+)")
    if m2.group(3) != addr or m2.group(2) != "untouched":
        raise GuestError(f"isolation broken: reader saw {m2.group(2)!r} at {m2.group(3)} (writer {addr})")
    since = d.g.mark()
    d.terminal("memtest peek ffffffff80000000")
    d.g.wait_for(r"@@PROC_EXIT pid=\d+ name=memtest status=11 reason=segmentation fault: read denied at ffffffff80000000", timeout=10, since=since)
    d.terminal("memtest peek ffff800000100000")
    d.g.wait_for(r"@@PROC_EXIT pid=\d+ name=memtest status=11 reason=segmentation fault: read denied at ffff800000100000", timeout=10, since=since)
    d.g.wait_for(rf"@@TERM memtest: pid {writer} still sees SECRET-42", timeout=15, since=since)
    d.g.wait_for(rf"@@PROC_EXIT pid={writer} name=memtest status=0", timeout=10, since=since)
    return f"same VA {addr}: writer kept SECRET-42, reader saw its own 'untouched'; kernel/HHDM reads faulted"


def s_concurrency(d):
    term_focus(d)
    since = d.g.mark()
    d.terminal("spin &")
    spin = d.g.wait_for(r"@@PROC_START pid=(\d+) name=spin", timeout=10, since=since).group(1)
    d.terminal("counter 5 200 B &")
    # The CPU hog does not starve sleeping programs or the desktop.
    d.g.wait_for(r"@@TERM B 5\s*$", timeout=20, since=since)
    d.g.wait_for(r"@@PROC_EXIT pid=\d+ name=counter status=0", timeout=10, since=since)
    d.g.wait_for(r"@@TERM spin: \d+ M iterations after [2-9]\d\d\d ms", timeout=20, since=since)
    s2 = d.g.mark()
    d.dock_click("Files")
    d.wait_focus("Files", s2)
    term_focus(d)
    ps_row = rf"@@TERM +{spin} +\d+ +\d+ +\d+ running +(\d+)"
    d.terminal("ps", ps_row)
    cpu = int(re.findall(ps_row, d.g.serial())[-1])
    since = d.g.mark()
    d.terminal(f"kill {spin}")
    d.g.wait_for(rf"@@PROC_EXIT pid={spin} name=spin status=9 reason=killed by SIGKILL", timeout=10, since=since)
    since = d.g.mark()
    d.terminal("ps", r"@@TERM context switches:")
    if re.search(rf"@@TERM +{spin} ", d.g.serial()[since:]):
        raise GuestError("spin still listed after kill")
    if not re.search(r"@@TERM +1 +0 +0 +1 running .* init", d.g.serial()[since:]):
        raise GuestError("init (pid 1) missing from ps")
    since = d.g.mark()
    d.dock_click("Files")
    d.wait_focus("Files", since)
    since = d.g.mark()
    d.g.key("ctrl", "w")  # close Files
    d.g.wait_for(r"@@WIN_CLOSE id=\d+", timeout=10, since=since)
    return f"spin used {cpu} ms CPU while counter finished and the desktop opened Files; kill worked"


def s_crash_does_not_stop_others(d):
    term_focus(d)
    since = d.g.mark()
    d.terminal("counter 8 250 C &")
    d.g.wait_for(r"@@TERM C 2\s*$", timeout=10, since=since)
    d.terminal("crash")
    d.g.wait_for(r"@@PROC_EXIT pid=\d+ name=crash status=11", timeout=10, since=since)
    d.g.wait_for(r"@@TERM C 8\s*$", timeout=15, since=since)
    d.g.wait_for(r"@@PROC_EXIT pid=\d+ name=counter status=0", timeout=10, since=since)
    return "counter kept running through another process's crash and exited 0"


def s_stdin_and_shell(d):
    """0.6: programs run on the Terminal's pty. The line discipline echoes
    and edits input (Backspace), Ctrl+D is end-of-file, and Ctrl+C sends
    SIGINT to the foreground process group: it kills a program running
    under the shell while the shell survives."""
    term_focus(d)
    since = d.g.mark()
    d.terminal("upper", r"@@TERM upper: type lines")
    d.g.type_text("mixed Casx")
    d.g.key("backspace")
    d.g.type_text("e 123\n")
    d.g.wait_for(r"@@TERM MIXED CASE 123\s*$", timeout=10, since=since)
    d.g.key("ctrl", "d")
    d.g.wait_for(r"@@TERM upper: 1 lines", timeout=10, since=since)
    since = d.g.mark()
    d.terminal("sh", r"@@TERM Aurora sh \(pid \d+\)")
    time.sleep(0.5)  # the prompt is an unterminated line: no TERM marker
    d.g.type_text("hello nested\n")
    d.g.wait_for(r"@@TERM Hello from user space! pid=\d+ argc=2 \[nested\]", timeout=10, since=since)
    d.g.type_text("cat /system/welcome.txt | grep -c Aurora\n")
    d.g.wait_for(r"@@TERM [1-9]\d*\s*$", timeout=10, since=since)
    # Ctrl+C kills the foreground program; the shell survives.
    since = d.g.mark()
    d.g.type_text("spin\n")
    d.g.wait_for(r"@@PROC_EXEC pid=\d+ name=spin", timeout=10, since=since)
    time.sleep(1.0)
    d.g.key("ctrl", "c")
    d.g.wait_for(r"@@PTY_SIGNAL pty=\d+ sig=2", timeout=10, since=since)
    d.g.wait_for(r"@@PROC_EXIT pid=\d+ name=spin status=2 reason=killed by SIGINT", timeout=10, since=since)
    since = d.g.mark()
    d.g.type_text("echo still-here\n")
    d.g.wait_for(r"@@TERM still-here\s*$", timeout=10, since=since)
    d.g.type_text("exit\n")
    d.g.wait_for(r"@@PROC_EXIT pid=\d+ name=sh status=0", timeout=10, since=since)
    # Ctrl+C on a program started directly by the Terminal
    since = d.g.mark()
    d.terminal("spin")
    d.g.wait_for(r"@@PROC_START pid=\d+ name=spin", timeout=10, since=since)
    time.sleep(1.0)
    d.g.key("ctrl", "c")
    d.g.wait_for(r"@@PROC_EXIT pid=\d+ name=spin status=2 reason=killed by SIGINT", timeout=10, since=since)
    return ("pty echo + Backspace + Ctrl+D reached upper; sh ran a pipeline; Ctrl+C killed spin under sh "
            "(SIGINT to the foreground group) and the shell kept working")


def s_process_leaks(d):
    term_focus(d)
    m0 = d.terminal("mem", r"@@TERM Physical pages free: +(\d+) KiB")
    before = int(m0.group(1))
    for i in range(10):
        since = d.g.mark()
        d.terminal("hello leakcheck")
        d.g.wait_for(r"@@PROC_EXIT pid=\d+ name=hello status=0", timeout=10, since=since)
    m1 = d.terminal("mem", r"@@TERM Physical pages free: +(\d+) KiB")
    after = int(m1.group(1))
    if after < before - 64:
        raise GuestError(f"physical memory leaked across 10 runs: {before} -> {after} KiB free")
    return f"10 process lifetimes: free physical memory {before} -> {after} KiB"


WM_MIN_W, WM_MIN_H = 320, 180  # kernel/include/desktop/desktop.h


def s_window_resize(d):
    open_notes_fresh(d)
    w = d.window("Notes")
    if w["state"] != "normal":
        raise GuestError(f"Notes is {w['state']}")
    gx, gy = w["x"] + w["w"] - 4, w["y"] + w["h"] - 4
    results = []
    for nw, nh in ((w["w"] - 120, w["h"] - 80), (WM_MIN_W, WM_MIN_H), (w["w"], w["h"])):
        # the window's lower-right pixel lands under the cursor on release
        tx, ty = w["x"] + nw - 1, w["y"] + nh - 1
        if nw == WM_MIN_W:  # drag past the minimum: it clamps
            tx, ty = w["x"] + 40, w["y"] + 40
        since = d.g.mark()
        d.drag(gx, gy, tx, ty)
        m = d.g.wait_for(r"@@WIN id=\d+ app=Notes x=-?\d+ y=-?\d+ w=(\d+) h=(\d+) state=normal", timeout=10, since=since)
        got = (int(m.group(1)), int(m.group(2)))
        if got != (nw, nh):
            raise GuestError(f"resize to {(nw, nh)} gave {got}")
        results.append(got)
        gx, gy = w["x"] + got[0] - 4, w["y"] + got[1] - 4
    # the resized window still works
    d.g.type_text("ok")
    d.g.screendump(OUT / "resized.png")
    since = d.g.mark()
    d.g.key("ctrl", "n")
    d.g.wait_for(r"@@NOTES_DIALOG unsaved", timeout=10, since=since)
    d.g.key("d")
    return f"grip drags gave {results} (minimum clamp {WM_MIN_W}x{WM_MIN_H})"


CLIP_TEXT = "copy me please\nsecond line"


def s_clipboard(d):
    open_notes_fresh(d)
    d.g.type_text(CLIP_TEXT)
    # Shift+Home selects the last line; Ctrl+C copies it.
    d.g.key("shift", "home")
    since = d.g.mark()
    d.g.key("ctrl", "c")
    m = d.g.wait_for(r"@@CLIP_SET len=(\d+) crc=([0-9a-f]+)", timeout=10, since=since)
    if (int(m.group(1)), m.group(2)) != (11, crc("second line")):
        raise GuestError(f"copied {m.group(0)}, expected 'second line'")
    # Typing replaces the selection; Ctrl+A, Ctrl+X empties the note.
    d.g.type_text("2nd")
    d.g.key("ctrl", "a")
    since = d.g.mark()
    d.g.key("ctrl", "x")
    m = d.g.wait_for(r"@@CLIP_SET len=(\d+) crc=([0-9a-f]+)", timeout=10, since=since)
    want = "copy me please\n2nd"
    if (int(m.group(1)), m.group(2)) != (len(want), crc(want)):
        raise GuestError(f"cut {m.group(0)}, expected {want!r}")
    # Paste twice: the note must be exactly two copies.
    d.g.key("ctrl", "v")
    d.g.key("ctrl", "v")
    m = save_as(d, "clip.txt")
    if m.group(2) != "0" or int(m.group(3)) != 2 * len(want) or m.group(4) != crc(want + want):
        raise GuestError(f"pasted note differs: {m.group(0)}")
    # Mouse selection: double-click a word, then drag across a range.
    def text_pt(col, row):  # kernel/apps/notes.c: PAD 14, text at TOOLBAR_H + 8, LINE_H 18
        return d.content_point("Notes", 14 + 8 * col, TOOLBAR_H + 8 + 18 * row + 9)
    d.click(*text_pt(1, 0), double=True)
    since = d.g.mark()
    d.g.key("ctrl", "c")
    m = d.g.wait_for(r"@@CLIP_SET len=(\d+) crc=([0-9a-f]+)", timeout=10, since=since)
    if (int(m.group(1)), m.group(2)) != (4, crc("copy")):
        raise GuestError(f"double-click copied {m.group(0)}, expected 'copy'")
    d.drag(*text_pt(5, 0), *text_pt(14, 0))
    since = d.g.mark()
    d.g.key("ctrl", "c")
    m = d.g.wait_for(r"@@CLIP_SET len=(\d+) crc=([0-9a-f]+)", timeout=10, since=since)
    if (int(m.group(1)), m.group(2)) != (9, crc("me please")):
        raise GuestError(f"drag copied {m.group(0)}, expected 'me please'")
    d.g.key("ctrl", "a")
    since = d.g.mark()
    d.g.key("ctrl", "c")  # restore the clipboard for the Terminal check
    d.g.wait_for(r"@@CLIP_SET len=36 ", timeout=10, since=since)
    # Terminal paste takes the first line only.
    term_focus(d)
    d.g.type_text("echo [")
    since = d.g.mark()
    d.g.key("ctrl", "v")
    d.g.wait_for(r"@@TERM_PASTE added=14 ", timeout=10, since=since)
    d.g.type_text("]\n")
    d.g.wait_for(r"@@TERM \[copy me please\]\s*$", timeout=10, since=since)
    d.terminal("rm /home/notes/clip.txt")
    return (f"copy/cut/paste in Notes ({2 * len(want)} B saved, crc ok); double-click and drag selection; "
            "Terminal pasted the first line")


def s_caps_led(d):
    term_focus(d)
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
    return "Caps Lock set and cleared the keyboard LED; letters followed it"


def s_poweroff(d):
    term_focus(d)
    since = d.g.mark()
    d.g.type_text("poweroff\n")
    d.g.wait_for(r"@@POWEROFF", timeout=10, since=since)
    t0 = time.time()
    while d.g.alive() and time.time() - t0 < 20:
        time.sleep(0.1)
    if d.g.alive():
        raise GuestError("QEMU still running 20 s after poweroff")
    if "acpi: entering S5" not in d.g.serial():
        raise GuestError("power-off did not go through ACPI S5")
    return f"ACPI S5: QEMU exited {time.time() - t0:.1f} s after the command"


SCENARIOS = [
    ("launch-each-app", s_launch_each_app),
    ("drag-overlap-focus", s_drag_overlap_focus),
    ("minimize-restore", s_minimize_restore),
    ("notes-save-reopen-cat", s_notes_roundtrip),
    ("readonly-save-keeps-edits", s_readonly_save),
    ("unsaved-dialog-save", s_unsaved_save_path),
    ("theme-switch", s_theme_switch),
    ("terminal-errors", s_terminal_errors),
    ("input-limits", s_limits),
    ("cursor-edges", s_edges),
    ("menus", s_menus),
    ("open-close-memory", s_open_close_leak),
    ("user-programs-and-faults", s_user_programs),
    ("process-memory-isolation", s_memory_isolation),
    ("preemptive-concurrency", s_concurrency),
    ("crash-does-not-stop-others", s_crash_does_not_stop_others),
    ("stdin-shell-ctrl-c", s_stdin_and_shell),
    ("process-memory-reclaimed", s_process_leaks),
    ("window-resize", s_window_resize),
    ("clipboard-copy-paste", s_clipboard),
    ("caps-lock-led", s_caps_led),
    ("reboot-resets-ramfs", s_reboot),
    ("acpi-poweroff", s_poweroff),  # last: ends the guest
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--soak", type=int, default=0, help="run only the soak test for N seconds")
    ap.add_argument("--memory", default="256M")
    ap.add_argument("--smp", type=int, default=None)
    args = ap.parse_args()
    if not IMG.exists():
        sys.exit("error: build/test/aurora.img missing; run 'python tools/build.py build --variant test'")
    OUT.mkdir(parents=True, exist_ok=True)
    results = []
    g = Guest(IMG, memory=args.memory, name="interaction", no_reboot=False, keep_dir=OUT / "interaction", smp=args.smp)
    try:
        g.start()
        d = Desktop(g)
        d.boot()
        # Dismiss the welcome window with Enter (it has keyboard focus).
        g.key("ret")
        g.wait_for(r"@@WIN_CLOSE id=1", timeout=10)
        scenarios = [("soak", lambda d: s_soak(d, args.soak))] if args.soak else SCENARIOS
        for name, fn in scenarios:
            if args.only and name not in args.only:
                continue
            t0 = time.time()
            try:
                detail = fn(d)
                results.append({"test": name, "result": "pass", "detail": detail, "seconds": round(time.time() - t0, 1)})
                print(f"PASS  {name}: {detail}")
            except GuestError as e:
                msg = str(e)
                results.append({"test": name, "result": "fail", "detail": msg.splitlines()[0], "seconds": round(time.time() - t0, 1)})
                print(f"FAIL  {name}: {msg[:1500]}")
                try:
                    g.screendump(OUT / f"fail-{name}.png")
                except Exception:
                    pass
                if not g.alive():
                    break
    finally:
        g.stop()
    name = "soak.json" if args.soak else "interaction.json"
    (OUT / name).write_text(json.dumps(results, indent=2))
    failed = [r for r in results if r["result"] != "pass"]
    print(f"interaction: {len(results) - len(failed)}/{len(results)} passed")
    sys.exit(1 if failed or not results else 0)


if __name__ == "__main__":
    main()
