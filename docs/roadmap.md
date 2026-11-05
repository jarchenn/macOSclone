# Roadmap

Release 0.1 is complete when its acceptance matrix in `STATUS.md` passes.
Later milestones, in priority order, each with its own acceptance criteria:

## 0.2 — User-process isolation

Prerequisites: 0.1 kernel, heap and VMM.

* Task structures, per-task kernel stacks, TSS.RSP0, preemptive scheduling on
  the timer (move to LAPIC timer + IOAPIC, keeping PIC as fallback).
* Ring-3 GDT segments, `syscall`/`sysret`, a minimal syscall table with
  strict user-pointer and length validation.
* Per-process page tables (shared higher half), ELF64 validation/loading.
* Move apps out of the kernel only after syscalls and IPC work; the desktop
  becomes a user-space service with explicit window/surface APIs.
* Tests: a crashing process does not crash the kernel; one process cannot read
  another's memory.

## 0.3 — Persistent storage

* VirtIO block driver (virtqueue ownership, barriers, timeouts, error paths)
  on a separate disposable data disk.
* A simple journaled or copy-on-write filesystem behind the existing VFS.
* Explicit flush semantics; reboot tests prove saved content survives.
  No crash-consistency claims until power-loss tests exist.

## 0.4 — Networking

* One QEMU NIC (virtio-net or e1000), then ARP, IPv4, ICMP, UDP, DHCP, DNS,
  TCP, each as a tested milestone; a basic HTTP/1.1 client. No TLS claims.

## Later

Richer text rendering (proportional/anti-aliased fonts), clipboard, file
dialogs, window resizing, delete/rename in the VFS, keyboard layouts and LEDs,
audio, USB HID, SMP, ACPI power-off, more graphics modes.

Out of scope as independent research-scale projects: browser engine, package
ecosystem, Apple Silicon port, macOS binary compatibility.
