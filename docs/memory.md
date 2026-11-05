# Memory

## Physical vs virtual

`phys_addr_t` values are plain integers and are never dereferenced. They are
converted with `phys_to_virt()` (adds the Limine HHDM offset). All page-table
and allocator code goes through this conversion.

## Physical allocator (`memory/physical.c`)

* One bit per 4 KiB page from address 0 to the highest *usable* or
  *bootloader-reclaimable* byte (1 = in use). The bitmap is placed in the first
  usable region large enough and marks itself used.
* Initially everything is used; only `USABLE` ranges (shrunk inwards to whole
  pages) are freed. Page 0 is never handed out, so `0` means "no page".
* Reserved, ACPI, framebuffer, bad and kernel/module memory are never in the
  free set. `BOOTLOADER_RECLAIMABLE` memory is freed only after
  `kmain_stage2` runs on Aurora's own stack and page tables, when every
  bootloader response has been copied (`pmm_reclaim_bootloader`).
* Allocation is next-fit over 64-bit words; pages are zeroed on allocation.
* `pmm_free_page` panics on unaligned / out-of-range / bitmap-owned addresses
  and on double frees (the portable core reports them as status codes, which
  the host tests check).

## Virtual layout (4-level paging)

| Range | Mapping | Permissions |
|---|---|---|
| `0x0` – `0x00007fffffffffff` | nothing | NULL and all low addresses fault |
| HHDM (Limine offset, `0xffff800000000000` on QEMU) | usable, bootloader-reclaimable and kernel+module RAM (2 MiB pages where aligned) | RW, NX |
| HHDM + framebuffer phys | framebuffer, 4 KiB pages | RW, NX, WC (PAT5) or UC |
| `0xffffc00000000000` | kernel heap, mapped on demand | RW, NX |
| `0xffffd00000000000` | guard page (unmapped) | — |
| `0xffffd00000001000` | 64 KiB kernel stack | RW, NX |
| `0xffffe00000000000` | local APIC MMIO page | RW, NX, UC |
| `0xffffffff80000000` | kernel image | `.limine_requests` RW+NX, `.text` RX, `.rodata` R+NX, `.data/.bss` RW+NX |

ACPI and reserved regions are deliberately not mapped. The kernel checks that
the HHDM does not collide with the heap region before switching.

NX is set only if CPUID reports it and Limine enabled `EFER.NXE`; otherwise
the bit would be a reserved-bit fault. CR0.WP (set by Limine) makes RO kernel
pages read-only for the kernel too.

## Cache policy

Limine documents its PAT layout (PAT0 = WB … PAT5 = WC). Aurora does **not**
reprogram the PAT. Before using write-combining for the framebuffer it reads
`IA32_PAT` and checks that entry 5 is WC (type 1); otherwise it uses
PCD|PWT (uncached). The chosen mode is logged and shown in About. On QEMU
the check passes (`fb=768 pages (write-combining)`).

## TLB

The CR3 load flushes all non-global entries (no global pages are used).
`paging_unmap_4k` issues `invlpg`. New mappings of previously non-present
pages need no invalidation.

## Stacks

| Stack | Size | Location |
|---|---|---|
| Limine boot stack | ≥ 64 KiB | used only until `switch_stack_and_call`, then reclaimed |
| Kernel stack | 64 KiB | `0xffffd00000001000`, guard page below |
| IST1 (#DF) | 16 KiB | kernel .bss |
| IST2 (NMI, #MC) | 16 KiB | kernel .bss |

A kernel stack overflow faults on the guard page; the resulting #DF runs on
IST1 and reaches the panic handler.

## Heap (`memory/heap.c`, `memory/kmalloc.c`)

* Boundary-tag allocator: 32-byte headers with magic, size and previous-block
  size; payloads 16-byte aligned; O(1) coalescing with both neighbours;
  first-fit free list.
* Grows in ≥ 64 KiB steps by mapping fresh pages; growth failure rolls back.
* Bounded: limit = usable RAM (after reclaiming bootloader memory) / 3,
  clamped to 8–96 MiB. Measured under QEMU/OVMF: 40 MiB with `-m 128M`,
  83 MiB with `-m 256M`, 96 MiB with `-m 512M`. Requests over 64 MiB,
  zero-size requests and arithmetic overflow return NULL.
* Invalid frees, interior pointers and double frees are detected (kernel:
  panic). `heap_check` walks every block and cross-checks the statistics.
* Statistics (`mem` in Terminal, About): mapped bytes, bytes in use, peak,
  live allocations, failures.
* Main-loop context only; never called from IRQ handlers.

No memory protection exists between apps in 0.1.
