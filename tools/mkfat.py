#!/usr/bin/env python3
"""Build an MBR-partitioned raw disk image containing one FAT16 filesystem.

Pure Python, no external tools. Supports long file names (VFAT LFN entries) so
that names such as ``limine-bios.sys`` are stored exactly.

Layout:
  LBA 0            MBR (partition table; Limine may later install BIOS stage1)
  LBA 1..2047      gap (used by ``limine bios-install`` for stage 2)
  LBA 2048..       FAT16 partition, type 0xEF (EFI System Partition)
"""
import struct
import sys
from pathlib import Path

SECTOR = 512
PART_START = 2048


def _short_name_ok(name: str) -> bool:
    if name.upper() != name and name.lower() != name:
        return False
    base, dot, ext = name.partition(".")
    if not base or len(base) > 8 or len(ext) > 3 or "." in ext:
        return False
    allowed = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")
    return all(c in allowed for c in (base + ext).upper()) and name == name.upper()


def _short_entry_name(base: str, ext: str) -> bytes:
    return base.ljust(8)[:8].encode("ascii") + ext.ljust(3)[:3].encode("ascii")


def _lfn_checksum(short: bytes) -> int:
    s = 0
    for b in short:
        s = (((s & 1) << 7) | (s >> 1)) + b
        s &= 0xFF
    return s


class _Dir:
    def __init__(self):
        self.children = {}  # name -> _Dir | bytes
        self.cluster = 0


def _make_short(name: str, used: set) -> bytes:
    if _short_name_ok(name):
        base, _, ext = name.partition(".")
        short = _short_entry_name(base, ext)
        if short in used:
            raise ValueError(f"duplicate short name for {name}")
        used.add(short)
        return short
    base, _, ext = name.rpartition(".") if "." in name else (name, "", "")
    clean = lambda s: "".join(c for c in s.upper() if c.isalnum() or c in "_-")
    base, ext = clean(base) or "FILE", clean(ext)[:3]
    for n in range(1, 100):
        tail = f"~{n}"
        short = _short_entry_name(base[: 8 - len(tail)] + tail, ext)
        if short not in used:
            used.add(short)
            return short
    raise ValueError(f"cannot create short name for {name}")


def _dir_entries(d: _Dir, parent_cluster: int, is_root: bool, alloc_info) -> bytes:
    out = bytearray()
    if not is_root:
        out += struct.pack("<11sBBBHHHHHHHI", b".          ", 0x10, 0, 0, 0, 0, 0, 0, 0, 0, d.cluster, 0)
        out += struct.pack("<11sBBBHHHHHHHI", b"..         ", 0x10, 0, 0, 0, 0, 0, 0, 0, 0, parent_cluster, 0)
    used = set()
    for name in sorted(d.children):
        child = d.children[name]
        short = _make_short(name, used)
        if not _short_name_ok(name):
            csum = _lfn_checksum(short)
            units = [ord(c) for c in name] + [0x0000]
            while len(units) % 13:
                units.append(0xFFFF)
            chunks = [units[i:i + 13] for i in range(0, len(units), 13)]
            for idx in range(len(chunks), 0, -1):
                ch = chunks[idx - 1]
                seq = idx | (0x40 if idx == len(chunks) else 0)
                e = bytearray(32)
                e[0] = seq
                e[1:11] = struct.pack("<5H", *ch[0:5])
                e[11] = 0x0F
                e[12] = 0
                e[13] = csum
                e[14:26] = struct.pack("<6H", *ch[5:11])
                e[26:28] = b"\0\0"
                e[28:32] = struct.pack("<2H", *ch[11:13])
                out += e
        if isinstance(child, _Dir):
            attr, cluster, size = 0x10, child.cluster, 0
        else:
            attr, cluster, size = 0x20, alloc_info[id(child)] if child else 0, len(child)
        # Fixed timestamp (2026-01-01 00:00) for reproducible images.
        date = ((2026 - 1980) << 9) | (1 << 5) | 1
        out += struct.pack("<11sBBBHHHHHHHI", short, attr, 0, 0, 0, date, date, 0, 0, date, cluster, size)
    return bytes(out)


def build_image(files: dict, out_path: Path, part_mib: int = 64, label: str = "AURORA") -> None:
    """files: mapping of 'dir/sub/name' -> bytes."""
    root = _Dir()
    for path, data in files.items():
        parts = [p for p in path.split("/") if p]
        node = root
        for p in parts[:-1]:
            node = node.children.setdefault(p, _Dir())
            if not isinstance(node, _Dir):
                raise ValueError(f"{path}: parent is a file")
        node.children[parts[-1]] = bytes(data)

    total = part_mib * 1024 * 1024 // SECTOR
    spc = 4  # 2 KiB clusters
    reserved = 4
    nfats = 2
    root_entries = 512
    root_secs = root_entries * 32 // SECTOR
    fatsz = 1
    while True:
        data_secs = total - reserved - nfats * fatsz - root_secs
        clusters = data_secs // spc
        need = (clusters + 2) * 2
        new_fatsz = (need + SECTOR - 1) // SECTOR
        if new_fatsz == fatsz:
            break
        fatsz = new_fatsz
    if not (4085 <= clusters < 65525):
        raise ValueError("partition size not valid for FAT16")
    cluster_bytes = spc * SECTOR

    fat = [0] * (clusters + 2)
    fat[0], fat[1] = 0xFFF8, 0xFFFF
    data_area = {}
    next_cluster = [2]

    def alloc(nbytes: int) -> int:
        n = max(1, (nbytes + cluster_bytes - 1) // cluster_bytes)
        first = next_cluster[0]
        if first + n > clusters + 2:
            raise ValueError("image full")
        for i in range(n):
            c = first + i
            fat[c] = c + 1 if i + 1 < n else 0xFFFF
        next_cluster[0] += n
        return first

    file_clusters = {}

    # First pass: assign clusters to directories (size depends on entry count).
    def entry_count(d: _Dir, is_root: bool) -> int:
        n = 0 if is_root else 2
        for name in d.children:
            n += 1
            if not _short_name_ok(name):
                n += (len(name) + 1 + 12) // 13
        return n

    def assign(d: _Dir, is_root: bool):
        if not is_root:
            d.cluster = alloc(entry_count(d, False) * 32)
        for name in sorted(d.children):
            c = d.children[name]
            if isinstance(c, _Dir):
                assign(c, False)
            elif len(c):
                file_clusters[id(c)] = alloc(len(c))
                data_area[file_clusters[id(c)]] = c

    assign(root, True)
    if entry_count(root, True) > root_entries:
        raise ValueError("too many root entries")

    def emit_dirs(d: _Dir, parent: int, is_root: bool):
        blob = _dir_entries(d, parent, is_root, file_clusters)
        if is_root:
            data_area["root"] = blob
        else:
            data_area[d.cluster] = blob
        for name in sorted(d.children):
            c = d.children[name]
            if isinstance(c, _Dir):
                emit_dirs(c, 0 if is_root else d.cluster, False)

    emit_dirs(root, 0, True)

    part = bytearray(total * SECTOR)
    bs = bytearray(SECTOR)
    bs[0:3] = b"\xEB\x3C\x90"
    bs[3:11] = b"AURORAOS"
    struct.pack_into("<HBHBHHBHHHII", bs, 11, SECTOR, spc, reserved, nfats, root_entries,
                     0, 0xF8, fatsz, 63, 255, PART_START, total)
    struct.pack_into("<BBBI11s8s", bs, 36, 0x80, 0, 0x29, 0x41555230,
                     label.ljust(11)[:11].encode("ascii"), b"FAT16   ")
    bs[510:512] = b"\x55\xAA"
    part[0:SECTOR] = bs
    fat_bytes = struct.pack(f"<{len(fat)}H", *fat)
    for i in range(nfats):
        off = (reserved + i * fatsz) * SECTOR
        part[off:off + len(fat_bytes)] = fat_bytes
    root_off = (reserved + nfats * fatsz) * SECTOR
    blob = data_area.pop("root")
    part[root_off:root_off + len(blob)] = blob
    data_off = root_off + root_secs * SECTOR
    for cl, blob in data_area.items():
        off = data_off + (cl - 2) * cluster_bytes
        part[off:off + len(blob)] = blob

    mbr = bytearray(SECTOR)
    struct.pack_into("<I", mbr, 440, 0x41555230)  # disk signature
    entry = struct.pack("<B3sB3sII", 0x80, b"\xFE\xFF\xFF", 0xEF, b"\xFE\xFF\xFF", PART_START, total)
    mbr[446:446 + 16] = entry
    mbr[510:512] = b"\x55\xAA"

    with open(out_path, "wb") as f:
        f.write(mbr)
        f.write(bytes((PART_START - 1) * SECTOR))
        f.write(part)


if __name__ == "__main__":
    # Usage: mkfat.py out.img dest=src [dest=src ...]
    if len(sys.argv) < 3:
        sys.exit("usage: mkfat.py OUT.img DEST=SRC ...")
    mapping = {}
    for arg in sys.argv[2:]:
        dest, _, src = arg.partition("=")
        mapping[dest] = Path(src).read_bytes()
    build_image(mapping, Path(sys.argv[1]))
