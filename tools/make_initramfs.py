#!/usr/bin/env python3
"""Pack a directory tree into an Aurora initramfs archive (format "AURFS", v1).

Layout (all integers little-endian):

  header (32 bytes)
    0  char[8]  magic   "AURFS01\\0"
    8  u32      version  1
    12 u32      entry_count
    16 u64      total_size   (bytes, including header)
    24 u32      payload_crc32 (CRC-32/ISO-HDLC of bytes [32, total_size))
    28 u32      reserved (0)

  entries, repeated entry_count times, each 8-byte aligned:
    0  u32 type        1 = directory, 2 = regular file
    4  u32 path_len    1..255, path has no NUL, no leading '/'
    8  u64 data_len    0 for directories
    16 path bytes, zero padded to an 8-byte boundary
    .. data bytes, zero padded to an 8-byte boundary

Directories are emitted before their contents. The kernel validates every
field and rejects archives with impossible sizes or unsafe paths.
"""
import struct
import sys
import zlib
from pathlib import Path

MAGIC = b"AURFS01\0"
ALLOWED = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-")


def pad8(b: bytes) -> bytes:
    return b + bytes((-len(b)) % 8)


def check_component(c: str) -> None:
    if c in ("", ".", "..") or len(c) > 63 or any(ch not in ALLOWED for ch in c):
        raise SystemExit(f"error: invalid initramfs path component {c!r}")


def pack(src: Path) -> bytes:
    entries = []
    for p in sorted(src.rglob("*")):
        rel = p.relative_to(src).as_posix()
        if p.name == ".keep":
            continue
        for c in rel.split("/"):
            check_component(c)
        if p.is_dir():
            entries.append((1, rel, b""))
        elif p.is_file():
            data = p.read_bytes().replace(b"\r\n", b"\n")
            entries.append((2, rel, data))
    body = bytearray()
    for typ, rel, data in entries:
        pb = rel.encode("ascii")
        if not 1 <= len(pb) <= 255:
            raise SystemExit(f"error: path too long: {rel}")
        body += struct.pack("<IIQ", typ, len(pb), len(data))
        body += pad8(pb)
        body += pad8(data)
    total = 32 + len(body)
    crc = zlib.crc32(bytes(body)) & 0xFFFFFFFF
    header = MAGIC + struct.pack("<IIQII", 1, len(entries), total, crc, 0)
    return header + bytes(body)


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: make_initramfs.py SRC_DIR OUT_FILE", file=sys.stderr)
        return 2
    out = pack(Path(sys.argv[1]))
    Path(sys.argv[2]).write_bytes(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
