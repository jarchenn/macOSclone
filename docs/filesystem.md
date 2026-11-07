# Filesystem

## Namespace and persistence

| Mount | Backend | Writable | Lifetime |
|---|---|---|---|
| `/` | synthetic directory | no | — |
| `/system` | initramfs (boot module), data read in place | no | image contents |
| `/home` | PFS on the VirtIO data disk, if one is attached | yes | **survives restarts and power loss** |
| `/home` (no data disk) | RAM filesystem (kernel heap) | yes | until restart |

`make run` attaches a 32 MiB data disk (`build/run/data.img`, created when
missing) and a NIC. `--no-disk` boots without the disk. Without a disk,
**files are stored in memory and reset when Aurora restarts**. Notes, Files,
Settings and the Welcome window say which mode is active
(`app_storage_note()`). `df` in Terminal prints it too.

At boot, the archive's `home/` entries are loaded into RAM first. If a data
disk is found:

* **blank disk** (no PFS superblock in either slot, and no other magic): it
  is formatted, and the seeded files are written to it;
* **valid PFS**: the seeded files are dropped and the disk's files are
  loaded;
* **anything else** (bad CRCs in both slots, an unknown magic, an I/O error):
  the disk is **not touched and not reformatted**. `/home` stays in RAM, and
  the log and `df` say why. Test: `damaged_disk_is_not_reformatted`.

Notes, Files, Terminal and the Welcome window all go through the same VFS
API; there are no app-private file lists.

## Data disk: VirtIO block (`drivers/virtio_blk.c`)

Legacy VirtIO over PCI (I/O BAR0) with split virtqueues in physically
contiguous DMA memory. Requests are polled with a 5-second timeout. On a
timeout or device error, the disk is taken offline and later writes return
`EIO`; nothing hangs. Every commit ends with a FLUSH request.

## PFS v1 on-disk format (`fs/pfs.c`, portable)

4 KiB blocks. Blocks 0 and 1 are two superblock slots, each CRC-protected and
carrying a generation number. Everything else is the data area. A catalog
lists every directory and file: path, type, size, extent start, CRC-32 of the
content. Every file is one contiguous extent.

A commit runs after every mutation (write on close, create, mkdir, remove,
rename, truncate):

1. Write changed files into blocks the committed state does not use (shadow
   paging / copy-on-write). Unchanged files keep their extents.
2. Write a new catalog the same way.
3. Flush, write the superblock with generation + 1 into the *other* slot,
   then flush again.

A crash before step 3 completes leaves the old superblock, catalog and data
intact. A torn superblock fails its CRC, so mount picks the valid slot with
the highest generation. Mount checks the catalog's CRC and magic and each
extent's bounds, then reads every file into memory and checks it against its
CRC. A damaged file is skipped and counted, and the rest are kept.

Tests:

* host `pfs`: simulated power loss after *every* block write of a commit,
  plus torn writes, then remount and compare;
* host `vfs_disk`: the VFS on top of PFS;
* guest `storage_test.py`: reboot persistence, killing QEMU right after a
  Notes save, and 4 hard kills (QEMU process killed) during a stress loop.
  After each kill every file is either the old or the new version, never a
  mix.

Limits: 512 entries, 64 KiB per file, paths ≤ 255 bytes, disk ≥ 64 blocks.
The whole catalog is rewritten on each commit, so a commit costs O(number of
files). There is no journal replay, fsck or defragmentation; a disk with too
much free-space fragmentation reports `ENOSPC` even when the total free space
would fit.

## Paths

* Absolute paths start at `/`; Terminal resolves relative paths against its
  current directory, Notes' Save As against `/home/notes`.
* `.` is dropped, `..` removes a component, `..` at `/` stays at `/`;
  repeated and trailing slashes are ignored.
* Components: 1–63 characters from `[A-Za-z0-9._-]`, not `.`/`..`. Spaces are
  not allowed in names. Normalised paths are at most 255 bytes.

## Operations (`fs/vfs.h`)

`vfs_open/close/read/write/seek`, `vfs_stat`, `vfs_readdir` (index-based,
sorted by name), `vfs_create`, `vfs_mkdir`, `vfs_truncate`, and whole-file
`vfs_read_file` / `vfs_write_file`. Open flags: READ, WRITE, CREATE, TRUNC.
Offsets start at 0, advance by the bytes transferred; writing past the end
zero-fills the gap. Up to 32 open handles.

`vfs_write_file` replaces content atomically: new storage is allocated before
the old buffer is released, so on any error (read-only, too large, out of
space, out of memory) the previous content is unchanged. Notes relies on this
and also keeps its own buffer, so a failed save never loses edits.

`vfs_remove` deletes a file or an empty directory (`ENOTEMPTY` otherwise;
`EBUSY` if the file is open). `vfs_rename` moves a file or directory within
one filesystem. Since 0.6 it follows POSIX: an existing file (or empty
directory) at the target is replaced, and the removal and the move are one
PFS commit, so the rename is atomic on disk; file over directory is
`EISDIR`, directory over file `ENOTDIR`, an open target `EBUSY`, and moves
between `/home`, `/tmp` and `/` are `EXDEV`. Every node also has a mode, owner,
group and mtime (0.6, persisted in the PFS catalog; see
[userland.md](userland.md) for the permission checks). `vfs_sync`
forces a flush. Terminal exposes these as `rm`, `mv` and `sync`.

## Errors

| Code | Meaning |
|---|---|
| `ENOENT` | no such file or directory |
| `EEXIST` | file already exists |
| `ENOTDIR` | a path component is not a directory |
| `EISDIR` | directory opened/read as a file |
| `EROFS` | write to `/system` or `/` |
| `ENOSPC` | RAM storage full (2 MiB total or 512 nodes) |
| `EFBIG` | file over 64 KiB (or larger than the caller's buffer) |
| `EINVAL` | empty path, bad characters, relative path without cwd, bad flags |
| `ENAMETOOLONG` | component > 63 or path > 255 bytes |
| `EBADF` / `EMFILE` | invalid handle / more than 32 open files |
| `ENOMEM` | heap exhausted |
| `EBADARCHIVE` | invalid initramfs |
| `EBUSY` | file is open (remove/rename) |
| `ENOTEMPTY` | directory not empty (remove) |
| `EIO` | data disk error or offline |

## Limits

RAM files: 64 KiB per file, 2 MiB total reserved capacity, 512 nodes. Disk
files: 64 KiB per file and 512 entries, total limited by the disk size.
Notes edits at most 16 KiB; larger files are refused with a clear message.

## Initramfs format (AURFS v1)

Produced by `tools/make_initramfs.py` (format documented there): a 32-byte
header (magic `AURFS01\0`, version 1, entry count, total size, CRC-32 of the
payload) followed by 8-byte-aligned entries `{type, path_len, data_len, path,
data}`. The kernel parser (`fs/initramfs.c`) does not trust the module: it
checks the magic, version, declared size against the module size, the CRC,
every entry header against the remaining bytes, padding, entry type, that
directories carry no data, that paths are relative with valid non-dot
components, that parents exist before children, rejects duplicates, and
rejects trailing bytes. Host tests feed it truncated, corrupted, oversized and
path-traversal archives.
