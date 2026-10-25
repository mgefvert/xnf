# Kernel memory in XNF

XNF uses a small memblock table during boot, before it has a page allocator or
heap. The table stores sorted, non-overlapping, half-open physical ranges
`[base, base + length)` with 64-bit addresses. It lives in a fixed 128-entry
`.bss` array; E820 normalization uses a second fixed array of 66 boundary
addresses. No allocation is needed to manage either array.

## Before clearing .bss

XNF requires an i686 CPU (Pentium Pro or later). Real-mode assembly checks
CPUID availability and CPU family before C runs, matching `-march=i686`.
The initial real-mode stack and first protected-mode C stack share a 2 KiB
buffer inside the BIOS-loaded image. `boot_memory_check()` reads E820 on
that temporary stack, without using `.bss`, the final stack, the normal
console, or allocators. It rejects overflow and requires complete usable
coverage of the linker-defined kernel extent, with every enabled reserved
overlap taking precedence. Failure writes directly to VGA and halts with
IRQs disabled, leaving `.bss` and the final stack untouched. Success permits
switching to the final 16 KiB stack and clearing `.bss`.

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
PMM (which permanently retires `memblock_alloc()`), then initializes the VMM
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
A physical address is not a kernel
pointer, even when it is below 256 MiB. Zero indicates allocation failure.
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
before `.bss` is touched. `.bss` does not add
bytes to the BIOS-loaded image. Executable/data growth is still subject to
the existing 127-sector loader limit.

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
unaligned/out-of-range addresses,
unsupported flags, and invalid mapping state. Physical page zero cannot be
mapped. Flags are zero for read-only or `PAGING_WRITABLE`. These operations
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
on one CPU, no preemptive scheduler, and no IRQ/exception handler entering
the VMM. Reserved runs are private until fully initialized. Timer delivery
can continue during large allocations when the caller has enabled IRQs.
Add synchronization before introducing preemption or SMP. There are no SMP
locks or TLB shootdowns. The caller must stop using memory
before freeing it and must not separately free its backing frames through
PMM. As with ordinary pointer allocators, a stale pointer cannot be detected
after its address has been reused for a new allocation.

There is no `kmalloc`/`kfree` heap yet. Kernel section protection, stack guards,
demand paging, process address spaces, and swapping remain future work.

## Validation

Normal builds exclude self-tests and deliberate fault probes. Configure a
separate test build with the normal build's compiler/generator, then run:

```text
cmake -S . -B cmake-build-tests -DXNF_BUILD_TESTS=ON
cmake --build cmake-build-tests
python tests/boot_qemu.py --build cmake-build-debug
python tests/boot_qemu.py --build cmake-build-tests
python tests/pmm_qemu.py --build cmake-build-tests
python tests/vmm_qemu.py --build cmake-build-tests
```

Shared infrastructure lives in `tests/qemu_test.py`, with no argument parsing
or process execution at import. C test sources live in `tests/kernel/` and
are included only with `XNF_BUILD_TESTS=ON`; their declarations live in
`inc/kernel_test.h`, outside the public allocator interfaces.

The boot suite requires GDB. It checks ordinary 128/512 MiB boots, rejection
of 486/Pentium CPUs, the real legacy-response normalization branch, and
disabled reservations. Rejected reserved `.bss`/stack, gapped, disabled-RAM,
and overflowing layouts must leave sentinel bytes throughout `.bss` and the
final stack unchanged and remain halted without exceptions or resets.

The PMM suite checks every physical bitmap bit against an independent E820
model, 128/512 MiB boots, timers, faults/resets, overlapping/disjoint/partial
regions, disabled entries, the top frame below 4 GiB, and bootstrap retirement.
The top-frame fixture has exactly three allocatable frames, compared as a
set; tests do not depend on allocation order. PMM tests never dereference
frames. Synthetic E820
and retirement cases require GDB; use `--gdb PATH` if discovery fails.

The VMM suite requires GDB. It redirects disposable, normally booted guests
to explicit test entry points, so ordinary startup does not run exhaustive
tests. Real 128/512 MiB guests cover zeroing, data independence, page/table
boundaries, virtual reuse, noncontiguous physical frames, high physical RAM,
borrowed mappings, TLB remapping, permissions, invalid operations, physical
exhaustion with partial rollback, virtual exhaustion/fragmentation, and IF
preservation. A separate fixture exposes only high allocatable frames in real
512 MiB RAM, with a reserved hole guaranteeing noncontiguous backing. The
ordinary 512 MiB guest checks timer delivery during a large allocation.
Host checks compare the full PMM and VMM bitmaps before/after,
verify every kernel PTE and all future-user PDEs, and confirm timer progress.
Four further guests verify exact fault addresses/error codes and halted IRQ
state for null, freed, read-only-write, and user-boundary accesses. Logs and
snapshots stay in the build directory. VMM write fixtures describe only RAM
actually backed by the emulator; PMM-only fixtures may describe synthetic RAM.
