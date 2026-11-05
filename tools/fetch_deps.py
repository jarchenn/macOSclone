#!/usr/bin/env python3
"""Download and verify pinned third-party build dependencies.

Only data/binary release assets are fetched; nothing downloaded is executed by
this script. Every asset is verified against a pinned SHA-256 before use.
"""
import hashlib
import io
import shutil
import sys
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


def main() -> int:
    fetch_limine()
    return 0


if __name__ == "__main__":
    sys.exit(main())
