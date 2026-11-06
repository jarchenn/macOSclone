# Roadmap

Milestone status, with evidence in `STATUS.md`. "Done" means implemented and
tested in the guest. Partial items list exactly what is missing.

## 0.1: Bootable desktop (done)

Limine boot, kernel core, software desktop, four built-in apps, RAM `/home`.

## 0.2: User-process isolation (done, with one item deferred)

* Done: tasks with per-task kernel stacks and TSS.RSP0, preemptive
  round-robin scheduling of user mode on the PIT, ring-3 segments, an
  `int 0x80` syscall table with page-walk pointer validation, per-process page
  tables with a shared higher half, ELF64 validation and loading, a user
  libc, and 15 user programs. Faults kill only the faulting process. Tests
  show that one process cannot read another's memory.
* Deferred: **moving the desktop and apps out of the kernel** into a
  user-space service. This needs IPC and shared surfaces. The kernel is not
  preemptible (it yields explicitly), and the LAPIC timer and IOAPIC are not
  used; the PIT and 8259 remain.

## 0.3: Persistent storage (done)

A VirtIO block driver with timeouts, and PFS, a shadow-paged copy-on-write
filesystem on a separate data disk, with a commit after every change.
Power-loss tests run in the host tests (a crash after every write) and in
the guest (hard QEMU kills). A damaged disk is never reformatted.

## 0.4: Networking (done, client side)

virtio-net, ARP, IPv4, ICMP, UDP, DHCP, DNS, a TCP client and an HTTP/1.1
`fetch`. Not done: TCP listen/accept, congestion control, fragment
reassembly, IPv6, TLS.

## Later items

| Item | Status |
|---|---|
| Window resizing | **done**: drag the lower-right grip; an outline shows the new size; minimum 320×180 (`window-resize`) |
| Delete/rename in the VFS | **done**: `vfs_remove` and `vfs_rename`; Terminal `rm` and `mv`; persisted on disk |
| Clipboard | **done** (text): Notes selection with Shift+arrows, mouse drag and double-click; Ctrl+A/C/X/V; Terminal Ctrl+V pastes the first line. Guest-tested, including double-click and drag selection (`clipboard-copy-paste`). The clipboard is a kernel buffer, not a service |
| Keyboard LEDs | **done**: Caps Lock LED (`caps-lock-led`); Num Lock and Scroll Lock are not tracked |
| ACPI power-off | **done**: RSDP → RSDT/XSDT → FADT → `\_S5` found by a byte scan of the DSDT (no AML interpreter); Shut Down menu item and `poweroff`; syncs `/home` first (`acpi-poweroff`) |
| Keyboard layouts | not started (US only) |
| File dialogs | not started (Notes uses Save As with a name field; Open goes through Files) |
| Richer text (proportional, anti-aliased fonts) | not started (Spleen bitmap fonts) |
| More graphics modes | not started (uses the mode Limine sets) |
| Audio | not started |
| USB HID | not started (PS/2 only) |
| SMP | not started (one CPU) |
| FPU/SSE in user programs | not started (no FPU state is saved) |

Out of scope as independent research-scale projects: browser engine, package
ecosystem, Apple Silicon port, macOS binary compatibility.
