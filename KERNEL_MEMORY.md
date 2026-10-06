# Early physical memory in XNF

XNF uses a small memblock table during boot, before it has a page allocator or
heap. The table stores sorted, non-overlapping, half-open physical ranges
`[base, base + length)` with 64-bit addresses. It lives in a fixed 128-entry
`.bss` array; E820 normalization uses a second fixed array of 66 boundary
addresses. No allocation is needed to manage either array.

## Building the map

`memblock_init()` imports the BIOS E820 entries already collected by
`asm/kernel.asm`. Type 1 becomes `AVAILABLE`, type 3 becomes
`ACPI_RECLAIMABLE`, type 4 becomes `ACPI_NVS`, and all other reported
types become `RESERVED`. Gaps between reported ranges become `UNAVAILABLE`;
addresses beyond the final reported range are also unavailable to the
allocator. If E820 entries overlap, reserved memory takes precedence, then
ACPI NVS, ACPI reclaimable, and available RAM. Adjacent regions of the same
type are merged.

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
  addresses. `display_e820_map()` still prints the original BIOS entries.

Allocations are limited to `[4096, 0x100000000)`, since the current
32-bit flat kernel cannot directly address physical memory at or above 4 GiB.
E820 ranges above 4 GiB remain in the map without truncation. A successful
allocation never returns address zero. No memory is cleared by
`memblock_alloc()`; callers must initialize it if needed.

`kernel_main()` currently dumps the map, reserves 100 bytes with 16-byte
alignment and 8192 bytes with 4096-byte alignment, prints their addresses,
then dumps the map again. These are temporary boot diagnostics and consume
their ranges for that boot.

## Failure behavior

Initialization stops with a printed `Memblock panic` if E820 collection is
missing or incomplete, an E820 end address overflows 64 bits, the kernel
range is not fully available RAM, a map invariant fails, or the fixed table
would overflow. A zero-length E820 entry is ignored. Invalid allocation
requests (zero size, zero or non-power-of-two alignment, or use before
initialization) also panic. A valid request with no fitting available range
returns zero and leaves the map unchanged.

The table preserves byte ranges rather than rounding E820 data to pages.
It does not provide freeing, paging, guard pages, a heap, or a final physical
page allocator.
