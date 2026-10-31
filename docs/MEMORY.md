# Kernel memory in XNF

Memory management has four ownership layers:

| Layer | Responsibility | Result |
| --- | --- | --- |
| Memblock (`src/memory.c`) | Normalize firmware ranges and reserve bootstrap memory | Physical byte ranges |
| PMM (`src/pmm.c`) | Allocate/free individual available frames | Physical addresses |
| VMM (`src/vmm.c`) | Own virtual ranges and map PMM frames into them | Usable kernel pointers |
| Heap (`src/kheap.c`) | Divide VMM extents into aligned byte-sized objects | Object payload pointers |

`src/paging.c` supplies translation mechanics to VMM, without owning frames.
See [HEAP.md](HEAP.md) for byte-granularity allocation. Start with the
[overview](OVERVIEW.md); build/test commands are in
[BUILD.md](BUILD.md).

## Bootstrap region storage

XNF uses a small memblock table during boot, before it has a page allocator or
heap. The table stores sorted, non-overlapping, half-open physical ranges
`[base, base + length)` with 64-bit addresses. It lives in a fixed 128-entry
`.bss` array; E820 normalization uses a second fixed array of 66 boundary
addresses. No allocation is needed to manage either array.

## Early safety check

Before `.bss` is cleared or the final stack is used, `boot_memory_check()`
requires the whole kernel extent to be usable RAM. Enabled non-available
E820 overlaps and coverage holes reject boot. It uses the temporary loaded
stack, not allocator state. See [boot and CPU setup](BOOT.md) for the
entry sequence and early failure path.

## Building the map

`memblock_init()` imports the BIOS E820 entries already collected by
`asm/kernel.asm`. Type 1 becomes `AVAILABLE`, type 3 becomes
`ACPI_RECLAIMABLE`, type 4 becomes `ACPI_NVS`, and all other reported
types become `RESERVED`. Gaps between reported ranges become `UNAVAILABLE`;
addresses beyond the final reported range are also unavailable to the
allocator. If E820 entries overlap, reserved memory takes precedence, then
ACPI NVS, ACPI reclaimable, and available RAM. Adjacent regions of the same
type are merged.

Extended entries with attribute bit 0 clear are ignored, including their
lengths and types. BIOS legacy 20-byte responses are normalized by assembly
to attributes = 1 (enabled). The stored map preserves entry order but is not
byte-for-byte verbatim for legacy attributes.

The first physical page `[0, 4096)` is explicitly `RESERVED`. The linker
range `[__kernel_start, __kernel_end)` is then marked `KERNEL`. It includes
the loaded image, `.bss`, alignment gap, and bootstrap stack. This reservation
must be entirely within E820 available RAM; an overlap with firmware-reserved
or unknown memory stops boot.

## API

The declarations are in `inc/memory.h`:

- `void memblock_init(void)` builds the map once after `.bss` has been
  cleared and the console is usable.
- `uint64_t memblock_alloc(uint64_t size, uint64_t alignment)` finds the
  lowest suitable `AVAILABLE` range, aligns its start to a power of two,
  and changes the allocated portion to `BOOT_ALLOCATED`. It returns the
  physical base or zero if there is no suitable range. Allocations are
  permanent until a later memory manager takes ownership.
- `uint32_t memblock_region_count(void)` and
  `const MemblockRegion *memblock_region(uint32_t index)` expose the current
  sorted map for inspection. An out-of-range index returns a null pointer.
  Do not modify the returned region.
- `void memblock_dump(void)` prints the current map with exclusive end
  addresses. `display_e820_map()` prints collected, legacy-normalized entries.

Allocations are limited to `[4096, 0x100000000)`, since the current
32-bit flat kernel cannot directly address physical memory at or above 4 GiB.
E820 ranges above 4 GiB remain in the map without truncation. A successful
allocation never returns address zero. No memory is cleared by
`memblock_alloc()`; callers must initialize it if needed.

`kernel_main()` dumps the map, prepares and enables paging, initializes the
PMM (which permanently retires `memblock_alloc()`), then initializes VMM and heap
before entering the timer-driven idle loop. The PMM self-test runs only in
test builds. Ordinary boot makes no bootstrap allocations now that all paging
structures are static; the memblock allocator remains available for future
pre-paging needs and is still permanently retired by PMM handover.

## Failure behavior

Initialization stops with a printed `Memblock panic` if E820 collection is
missing or incomplete, an E820 end address overflows 64 bits, the kernel
range is not fully available RAM, a map invariant fails, or the fixed table
would overflow. A zero-length E820 entry is ignored. Invalid allocation
requests (zero size, zero or non-power-of-two alignment, or use before
initialization) also panic. A valid request with no fitting available range
returns zero and leaves the map unchanged.

The table preserves byte ranges rather than rounding E820 data to pages.
Retirement forbids new bootstrap allocations but preserves the table and all
existing reservations. PMM free validation continues to consult this map.

## Physical page ownership

`inc/pmm.h` provides allocation/freeing of individual 4 KiB physical frames.
Only complete pages of normalized available RAM below 4 GiB are managed.
Kernel, bootstrap, page-zero, and firmware reservations remain unavailable.
The 128 KiB PMM bitmap is part of the kernel's reserved `.bss`.

`pmm_alloc_page()` uses one circular bitmap search and one word hint. There
is no low/high physical split or promised allocation order, and no relation
to the kernel/user virtual boundary. It neither maps nor clears memory.
A physical address is not a kernel pointer, even when it is below 256 MiB.
Zero indicates allocation failure.
Only the owner may return an allocated frame through `pmm_free_page()`.

## Kernel virtual address space

The complete virtual interval `[0, 0x10000000)` belongs to the kernel.
Addresses at and above `0x10000000` are left unmapped for future processes.
This division does not reserve the same physical interval: a low kernel
virtual address may be backed by any PMM frame below 4 GiB.

- Page zero is permanently unmapped.
- `[4096, paging_identity_end)` is the permanent bootstrap identity map.
  Its end includes the entire linked kernel, stack, VGA, and all bootstrap
  allocations, rounded up to 4 MiB (with a 4 MiB minimum).
- `[paging_identity_end, 0x10000000)` is the dynamic VMM arena. Its unused
  pages have zero PTEs; virtual reservation does not allocate physical RAM.

Paging uses non-PAE, 4 KiB pages. All kernel mappings are supervisor-only.
`paging_prepare()` preallocates 64 page-aligned tables in `.bss`, costing
256 KiB of physical RAM. All 64 kernel PDEs are present, while the remaining
960 PDEs and unused PTEs remain zero. The page directory is also page-aligned
in `.bss`, costing another 4 KiB. Preparation allocates nothing. The required
identity extent includes any earlier bootstrap allocations and all static
structures, then is rounded up to the 4 MiB table boundary.

The tables and VMM metadata are included in the linker-defined kernel extent
and stay accessible through the permanent identity map. The linker rejects
an extent reaching beyond the start of the VGA aperture at `0xA0000`;
the early check and memblock additionally require the complete extent to be
E820-available RAM, so an earlier firmware reservation stops initialization
before `.bss` is touched. `.bss` does not add bytes to the BIOS-loaded image.
Executable/data growth is still subject to the existing 127-sector loader limit.

Activation sets CR0.PG and CR0.WP, loads CR3, and clears CR4.PAE/PSE. WP makes
supervisor writes honor read-only page permissions. The i686 baseline provides
the required CR4 and WP support.

`paging_query(address, &mapping)` returns the physical byte address and
effective RW permission for an existing kernel mapping. It returns false
for absent pages, the future user range, null output pointers, or use before
activation. Accessed/dirty bits do not affect the returned permissions.

`paging_internal.h` is an implementation interface for VMM and its tests:
map, unmap, and protect require ownership of the virtual slot in normal,
non-preemptive context. Each masks IRQs only for the individual PTE edit and
TLB invalidation, then restores IF. They reject permanent-prefix changes,
unaligned/out-of-range addresses, unsupported flags, and invalid mapping
state. Physical page zero cannot be mapped. Flags are zero for read-only or `PAGING_WRITABLE`. These operations
invalidate the affected TLB entry and never allocate/free physical frames.
They must not be used to bypass VMM ownership or modify live VMM allocations.

## Virtual page allocator

`inc/vmm.h` exposes:

- `vmm_init()`: initialize once after paging activation and PMM handover.
  Invalid initialization order or repeated initialization panics.
- `vmm_alloc_pages(count)`: return a page-aligned pointer to `count`
  virtually contiguous, zeroed, writable supervisor pages. Backing physical
  frames need not be contiguous. Returns null for zero/oversized requests,
  use before initialization, fragmented virtual space, or exhaustion.
- `vmm_free_pages(base)`: release the complete allocation at its original
  base. Returns false for null, unaligned/interior/out-of-range pointers,
  permanent mappings, use before initialization, and double frees.
- `vmm_free_page_count()`: report the number of free virtual arena pages
  (zero before initialization), separately from PMM physical counters.

Three fixed bitmaps record virtual occupancy and allocation starts/ends,
costing 24 KiB in `.bss`. First-fit searches find contiguous virtual runs.
Allocation reserves a run, acquires PMM frames, installs mappings, clears
through the new virtual addresses, and publishes allocation boundaries.
Physical exhaustion rolls back every partial mapping/frame and the virtual
reservation. Freeing validates the whole run, removes mappings and invalidates
translations before returning frames to PMM, then releases virtual ownership.
Metadata/mapping disagreement is an invariant failure and panics.

Operations preserve IF. Scans and zeroing leave IRQs in the caller's state;
only individual PMM operations and paging edits mask them. Transactions rely
on one CPU, non-preemptive normal context, and no IRQ/exception handler entering
the VMM. Reserved runs are private until fully initialized. Timer delivery
can continue during large allocations when the caller has enabled IRQs.
The experimental scheduler does not make VMM safe for competing threads;
callers must exclude preemption for the complete transaction. There are no SMP
locks or TLB shootdowns. The caller must stop using memory before freeing it
and must not separately free its backing frames through PMM. As with ordinary pointer allocators, a stale pointer cannot be detected
after its address has been reused for a new allocation.

The [heap](HEAP.md) now provides `kmalloc`, `kcalloc`, and `kfree` on top
of VMM. Kernel section protection, stack guards,
demand paging, process address spaces, and swapping remain future work.

## Reading the bit operations

The PMM has one bit for each physical frame. For frame number `f`, `f / 32`
selects its bitmap word and `f % 32` selects the bit inside that word. A set
bit means unavailable, either permanently reserved or currently allocated.
`~word` turns free bits into ones; counting its trailing zeros identifies a
free frame. This operation is used only when `~word` is nonzero.

A non-PAE virtual address has three fields:

| Bits | Meaning | C expression |
| --- | --- | --- |
| 31..22 | Directory index (10 bits) | `address >> 22` |
| 21..12 | Table index (10 bits) | `(address >> 12) & 1023` |
| 11..0 | Offset within a 4 KiB page | `address & 4095` |

A PTE stores a page-aligned physical base in bits 31..12 and flags below it.
Bit 0 is present; bit 1 permits writes; bit 2 permits user access and stays
clear in XNF. Effective write permission requires RW at both levels.
Masking with `0xFFFFF000` recovers the physical base. OR the original offset
back in to translate a byte address. For example, virtual `0x0040307B` uses
PDE 1, PTE 3, and offset `0x7B`; a PTE backed by frame `0x10002000` translates
to physical `0x1000207B`.

The CPU may cache translations in its TLB. PTE writes alone do not revoke
cached access: every runtime edit invalidates that virtual page with
`INVLPG`. Hardware accessed/dirty bits also explain why post-activation
checks mask those bits instead of comparing entries byte-for-byte.

## Allocating from C

```c
void *buffer = vmm_alloc_pages(3); /* 12 KiB of zeroed virtual memory. */
if (buffer != 0) {
    /* Use buffer as ordinary memory; its physical frames may be scattered. */
    vmm_free_pages(buffer);       /* Free the entire run using its original base. */
}
```

Call only after `vmm_init()` in normal non-preemptive execution context.
A frame number from `pmm_alloc_page()` must be mapped before it is used as
memory; use the heap for ordinary objects and VMM for page-sized interfaces.
Do not separately free a
VMM allocation's physical frames or modify its mappings through the internal
paging interface. See [validation coverage](BUILD.md#validation) for
exhaustion, rollback, permission, and ownership checks.
