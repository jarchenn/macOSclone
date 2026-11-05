# Filesystem

## Namespace and persistence

| Mount | Backend | Writable | Lifetime |
|---|---|---|---|
| `/` | synthetic directory | no | — |
| `/system` | initramfs (boot module), data read in place | no | image contents |
| `/home` | RAM filesystem (kernel heap) | yes | **until restart** |

**Files are stored in memory and reset when Aurora restarts.** Nothing is
written to disk. At boot the archive's `home/` entries are copied into the RAM
filesystem (currently `/home/notes/example.txt`) and `/home/notes` is ensured
to exist. Notes, Files, Terminal and the Welcome window all go through the
same VFS API; there are no app-private file lists.

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

There is no delete or rename in 0.1.

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

## Limits

RAM files: 64 KiB per file, 2 MiB total reserved capacity, 512 nodes.
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
