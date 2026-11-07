#!/usr/bin/env python3
"""Download and verify pinned third-party build dependencies.

Only data/binary release assets are fetched; nothing downloaded is executed by
this script. Every asset is verified against a pinned SHA-256 before use.
"""
import hashlib
import io
import shutil
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
THIRD_PARTY = ROOT / "third_party"

LIMINE_VERSION = "12.9.3"
LIMINE_URL = (
    "https://github.com/limine-bootloader/limine/releases/download/"
    f"v{LIMINE_VERSION}/limine-binary.zip"
)
LIMINE_SHA256 = "1bdbbcda650c5dba9b1eca5833d598137bf11bd6289bc89b6bcc4f8f112fa676"
LIMINE_DIR = THIRD_PARTY / f"limine-{LIMINE_VERSION}"

# Files we actually use from the Limine binary release.
LIMINE_REQUIRED = ["BOOTX64.EFI", "limine-bios.sys", "LICENSE", "limine.c"]


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def fetch(url: str) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": "aurora-os-build"})
    with urllib.request.urlopen(req, timeout=60) as resp:
        return resp.read()


def limine_ready() -> bool:
    stamp = LIMINE_DIR / ".verified"
    return stamp.exists() and all((LIMINE_DIR / f).exists() for f in LIMINE_REQUIRED)


def fetch_limine() -> None:
    if limine_ready():
        print(f"limine {LIMINE_VERSION}: already present in {LIMINE_DIR.relative_to(ROOT)}")
        return
    print(f"limine {LIMINE_VERSION}: downloading {LIMINE_URL}")
    try:
        data = fetch(LIMINE_URL)
    except Exception as exc:  # network errors are reported verbatim
        sys.exit(f"error: could not download Limine: {exc}")
    digest = sha256(data)
    if digest != LIMINE_SHA256:
        sys.exit(f"error: Limine SHA-256 mismatch: got {digest}, expected {LIMINE_SHA256}")
    tmp = LIMINE_DIR.with_suffix(".tmp")
    if tmp.exists():
        shutil.rmtree(tmp)
    tmp.mkdir(parents=True)
    with zipfile.ZipFile(io.BytesIO(data)) as zf:
        for info in zf.infolist():
            name = info.filename
            # Reject absolute paths and traversal inside the archive.
            parts = Path(name).parts
            if name.startswith(("/", "\\")) or ".." in parts:
                sys.exit(f"error: unsafe path in Limine archive: {name}")
            if not parts or parts[0] != "limine-binary":
                continue
            rel = Path(*parts[1:]) if len(parts) > 1 else None
            if rel is None or info.is_dir():
                continue
            dest = tmp / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(zf.read(info))
    for f in LIMINE_REQUIRED:
        if not (tmp / f).exists():
            sys.exit(f"error: Limine archive is missing {f}")
    if LIMINE_DIR.exists():
        shutil.rmtree(LIMINE_DIR)
    tmp.rename(LIMINE_DIR)
    (LIMINE_DIR / ".verified").write_text(LIMINE_SHA256 + "\n")
    print(f"limine {LIMINE_VERSION}: verified sha256 {digest}")


# Source archives for the user-space ports (0.6). Each is pinned to the
# digest the project publishes next to the release (newlib: sha512.sum on
# sourceware.org). Archives are only unpacked; their build scripts are never
# run: tools/build.py compiles the selected sources itself.
SOURCES = {
    "newlib": {
        "version": "4.6.0.20260123",
        "url": "https://sourceware.org/pub/newlib/newlib-4.6.0.20260123.tar.gz",
        "algo": "sha512",
        "digest": "ffa16d6465c0b429264c46395fa760fbcf072d3ff86e87330ba1f483efcfe66393ef83b0"
                  "3932759444a0ebeaef94d3ca58a59e91ab7b97b2a6ac6be2e7589657",
        "top": "newlib-4.6.0.20260123",
    },
    # Lua: SHA-256 as listed on https://www.lua.org/ftp/ and /tests/.
    "lua": {
        "version": "5.4.9",
        "url": "https://www.lua.org/ftp/lua-5.4.9.tar.gz",
        "algo": "sha256",
        "digest": "2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6",
        "top": "lua-5.4.9",
    },
    "lua-tests": {
        "version": "5.4.9",
        "url": "https://www.lua.org/tests/lua-5.4.9-tests.tar.gz",
        "algo": "sha256",
        "digest": "7d971845f545ffc09fbb3128a86b2c6524161c70d0fdf0154a16e8c00c343fca",
        "top": "lua-5.4.9-tests",
    },
}


def source_dir(name: str) -> Path:
    s = SOURCES[name]
    return THIRD_PARTY / f"{name}-{s['version']}"


def fetch_source(name: str) -> None:
    s = SOURCES[name]
    dest = source_dir(name)
    stamp = dest / ".verified"
    if stamp.exists() and stamp.read_text().strip() == s["digest"]:
        print(f"{name} {s['version']}: already present in {dest.relative_to(ROOT)}")
        return
    print(f"{name} {s['version']}: downloading {s['url']}")
    try:
        data = fetch(s["url"])
    except Exception as exc:
        sys.exit(f"error: could not download {name}: {exc}")
    digest = hashlib.new(s["algo"], data).hexdigest()
    if digest != s["digest"]:
        sys.exit(f"error: {name} {s['algo']} mismatch: got {digest}, expected {s['digest']}")
    tmp = dest.with_suffix(".tmp")
    if tmp.exists():
        shutil.rmtree(tmp)
    tmp.mkdir(parents=True)
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:*") as tf:
        for m in tf.getmembers():
            parts = Path(m.name).parts
            if m.name.startswith(("/", "\\")) or ".." in parts or not parts or parts[0] != s["top"]:
                sys.exit(f"error: unsafe path in {name} archive: {m.name}")
            if m.issym() or m.islnk():
                continue  # links are not needed and are never followed
            rel = Path(*parts[1:]) if len(parts) > 1 else None
            if rel is None:
                continue
            out = tmp / rel
            if m.isdir():
                out.mkdir(parents=True, exist_ok=True)
            elif m.isfile():
                out.parent.mkdir(parents=True, exist_ok=True)
                out.write_bytes(tf.extractfile(m).read())
    if dest.exists():
        shutil.rmtree(dest)
    tmp.rename(dest)
    stamp.write_text(s["digest"] + "\n")
    print(f"{name} {s['version']}: verified {s['algo']} {digest[:16]}...")


def main() -> int:
    fetch_limine()
    for name in SOURCES:
        fetch_source(name)
    return 0


if __name__ == "__main__":
    sys.exit(main())
