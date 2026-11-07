"""QEMU test harness: launches a guest with serial capture and a local QMP
socket, injects input, takes screenshots and always cleans up its process.

Every run uses a fresh temporary directory. QMP is bound to 127.0.0.1 on an
ephemeral port. Timeouts are bounded; waits poll observable state (serial
markers) rather than sleeping for fixed periods.
"""
import json
import os
import re
import shutil
import socket
import subprocess
import tempfile
import time
from pathlib import Path

import aurora_env as env


class GuestError(Exception):
    pass


def _free_port() -> int:
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


# qcode names for characters typed via QMP send-key
_SHIFTED = {
    '!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6', '&': '7', '*': '8', '(': '9', ')': '0',
    '_': 'minus', '+': 'equal', '{': 'bracket_left', '}': 'bracket_right', '|': 'backslash',
    ':': 'semicolon', '"': 'apostrophe', '~': 'grave_accent', '<': 'comma', '>': 'dot', '?': 'slash',
}
_PLAIN = {
    ' ': 'spc', '-': 'minus', '=': 'equal', '[': 'bracket_left', ']': 'bracket_right', '\\': 'backslash',
    ';': 'semicolon', "'": 'apostrophe', '`': 'grave_accent', ',': 'comma', '.': 'dot', '/': 'slash',
    '\n': 'ret', '\t': 'tab',
}


def char_keys(ch: str):
    if 'a' <= ch <= 'z' or '0' <= ch <= '9':
        return [ch]
    if 'A' <= ch <= 'Z':
        return ['shift', ch.lower()]
    if ch in _PLAIN:
        return [_PLAIN[ch]]
    if ch in _SHIFTED:
        return ['shift', _SHIFTED[ch]]
    raise ValueError(f"cannot type {ch!r}")


class Guest:
    def __init__(self, image, memory="256M", firmware="uefi", name="guest", no_reboot=True, keep_dir=None,
                 data_disk=None, network=None, smp=None):
        self.image = Path(image)
        self.memory = memory
        self.firmware = firmware
        self.no_reboot = no_reboot
        self.tmp = Path(tempfile.mkdtemp(prefix=f"aurora-{name}-"))
        self.keep_dir = Path(keep_dir) if keep_dir else None
        self.serial_path = self.tmp / "serial.log"
        self.proc = None
        self.qmp = None
        self._buf = b""
        self._pos = 0
        self.cursor = None  # last cursor position reported by the guest (test builds)
        self.data_disk = data_disk  # path to a raw image, kept across start/stop
        self.network = network
        # AURORA_TEST_SMP runs every suite with that many virtual CPUs.
        self.smp = smp if smp is not None else int(os.environ.get("AURORA_TEST_SMP", "1"))

    # ---- lifecycle -------------------------------------------------------
    def start(self):
        img = self.tmp / "disk.img"
        shutil.copyfile(self.image, img)  # never modify the built image
        vars_copy = None
        if self.firmware == "uefi":
            _, tmpl = env.ovmf()
            env.require("ovmf", tmpl)
            vars_copy = self.tmp / "vars.fd"
            shutil.copyfile(tmpl, vars_copy)
        port = _free_port()
        cmd = env.qemu_command(img, vars_copy, memory=self.memory, firmware=self.firmware,
                               serial=f"file:{self.serial_path}", qmp=f"tcp:127.0.0.1:{port},server=on,wait=off",
                               display="none", no_reboot=self.no_reboot,
                               data_disk=self.data_disk, network=self.network, smp=self.smp)
        self.cmdline = cmd
        self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, cwd=self.tmp)
        deadline = time.time() + 20
        while time.time() < deadline:
            if self.proc.poll() is not None:
                out = self.proc.stdout.read().decode(errors="replace")
                raise GuestError(f"QEMU exited early ({self.proc.returncode}): {out}")
            try:
                self.qmp = socket.create_connection(("127.0.0.1", port), timeout=2)
                break
            except OSError:
                time.sleep(0.1)
        else:
            raise GuestError("could not connect to QMP")
        self.qmp.settimeout(10)
        self._qmp_file = self.qmp.makefile("rwb")
        self._qmp_read()  # greeting
        self.command("qmp_capabilities")
        return self

    def stop(self):
        try:
            if self.qmp:
                try:
                    self.command("quit")
                except Exception:
                    pass
                self.qmp.close()
        finally:
            if self.proc and self.proc.poll() is None:
                self.proc.kill()
            if self.proc:
                try:
                    self.proc.wait(timeout=10)
                except Exception:
                    pass
            if self.keep_dir:
                self.keep_dir.mkdir(parents=True, exist_ok=True)
                if self.serial_path.exists():
                    shutil.copyfile(self.serial_path, self.keep_dir / "serial.log")
            shutil.rmtree(self.tmp, ignore_errors=True)

    def __enter__(self):
        return self.start()

    def __exit__(self, *a):
        self.stop()

    def alive(self):
        return self.proc and self.proc.poll() is None

    # ---- QMP -----------------------------------------------------------
    def _qmp_read(self):
        while True:
            line = self._qmp_file.readline()
            if not line:
                raise GuestError("QMP connection closed")
            msg = json.loads(line)
            if "event" in msg:
                if msg["event"] == "RESET":
                    self.resets = getattr(self, "resets", 0) + 1
                continue
            return msg

    def command(self, name, **args):
        req = {"execute": name}
        if args:
            req["arguments"] = args
        self._qmp_file.write((json.dumps(req) + "\n").encode())
        self._qmp_file.flush()
        resp = self._qmp_read()
        if "error" in resp:
            raise GuestError(f"QMP {name} failed: {resp['error']}")
        return resp.get("return")

    # ---- serial --------------------------------------------------------
    def serial(self) -> str:
        try:
            return self.serial_path.read_bytes().decode(errors="replace")
        except FileNotFoundError:
            return ""

    def wait_for(self, pattern, timeout=60, since=None, fail_on_panic=True):
        """Wait until regex `pattern` appears in serial output after offset
        `since` (default: the current read position). Returns the match."""
        rx = re.compile(pattern, re.M)
        start = self._pos if since is None else since
        deadline = time.time() + timeout
        while True:
            text = self.serial()
            m = rx.search(text, start)
            if m:
                self._pos = m.end()
                return m
            if fail_on_panic and "AURORA_PANIC" in text[start:]:
                raise GuestError("guest panicked:\n" + text[text.find("AURORA_PANIC", start) - 20:][:3000])
            if fail_on_panic and "LOCKDEP:" in text:
                # the lock validator (test build) found an ordering bug
                raise GuestError("lock validator report:\n" + text[text.find("LOCKDEP:") - 20:][:1500])
            if not self.alive():
                raise GuestError(f"QEMU exited while waiting for {pattern!r}\n--- serial tail ---\n{text[-3000:]}")
            if time.time() > deadline:
                raise GuestError(f"timeout waiting for {pattern!r}\n--- serial tail ---\n{text[-3000:]}")
            time.sleep(0.05)

    def mark(self):
        """Current end of serial output; use as `since` for later waits."""
        self._pos = len(self.serial())
        return self._pos

    # ---- input ---------------------------------------------------------
    def key(self, *keys, hold_ms=40):
        self.command("send-key", keys=[{"type": "qcode", "data": k} for k in keys], **{"hold-time": hold_ms})
        time.sleep(0.03)

    def type_text(self, text):
        for ch in text:
            self.key(*char_keys(ch))

    def mouse_move(self, dx, dy):
        """Relative PS/2 motion, split into small steps (the PS/2 protocol
        carries at most +-255 per packet)."""
        while dx or dy:
            sx = max(-60, min(60, dx))
            sy = max(-60, min(60, dy))
            self.command("input-send-event", events=[
                {"type": "rel", "data": {"axis": "x", "value": sx}},
                {"type": "rel", "data": {"axis": "y", "value": sy}}])
            dx -= sx
            dy -= sy
            time.sleep(0.01)

    def mouse_button(self, down, button="left"):
        self.command("input-send-event", events=[{"type": "btn", "data": {"down": down, "button": button}}])
        time.sleep(0.03)

    def mouse_wheel(self, steps):
        """Wheel clicks: positive = up (away from the user)."""
        button = "wheel-up" if steps > 0 else "wheel-down"
        for _ in range(abs(steps)):
            self.command("input-send-event", events=[{"type": "btn", "data": {"down": True, "button": button}}])
            self.command("input-send-event", events=[{"type": "btn", "data": {"down": False, "button": button}}])
            time.sleep(0.05)

    def screendump_ppm(self, path):
        """Raw screendump (PPM) for pixel comparisons (tools/imgdiff.py)."""
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.tmp / "shot.ppm"
        if tmp.exists():
            tmp.unlink()
        self.command("screendump", filename=str(tmp), format="ppm")
        deadline = time.time() + 10
        last = -1
        while time.time() < deadline:
            size = tmp.stat().st_size if tmp.exists() else 0
            if size and size == last:
                break
            last = size
            time.sleep(0.1)
        shutil.copyfile(tmp, path)
        return path

    def screendump(self, path):
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.tmp / "shot.png"
        self.command("screendump", filename=str(tmp), format="png")
        deadline = time.time() + 10
        while time.time() < deadline and (not tmp.exists() or tmp.stat().st_size == 0):
            time.sleep(0.05)
        time.sleep(0.2)
        shutil.copyfile(tmp, path)
        return path
