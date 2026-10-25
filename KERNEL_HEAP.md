# XNF kernel heap

[src/kheap.c](src/kheap.c) provides `kmalloc`, `kcalloc`, and `kfree` through
[inc/kheap.h](inc/kheap.h). It divides VMM allocations into byte-sized objects.
The heap owns those page allocations; callers own only their returned payloads.
Read [KERNEL_MEMORY.md](KERNEL_MEMORY.md) for the underlying PMM/VMM contracts
and [KERNEL_BUILD.md](KERNEL_BUILD.md#heap-checks) for the test suite.

## API and initialization

`kernel_main()` calls `kheap_init()` immediately after `vmm_init()`. Initialization
allocates no pages: it creates an empty heap, allowing boot even when a PMM-only
firmware fixture describes physical frames not actually backed by guest RAM.
Calling initialization again or before VMM initialization panics.

| Function | Contract |
| --- | --- |
| `kmalloc(size)` | At least `size` payload bytes, 16-byte aligned; contents unspecified |
| `kcalloc(count, size)` | Same alignment, with `count * size` payload bytes cleared |
| `kfree(pointer)` | Release one whole object using its exact live payload pointer |
| `kheap_allocated_pages()` | VMM pages currently owned by heap extents |
| `kheap_live_allocations()` | Number of live objects, excluding free blocks and metadata |

Zero-sized allocation, arithmetic overflow, allocation before initialization,
or insufficient resources returns null. Either zero argument to `kcalloc`
is rejected, and multiplication is checked before it occurs. Size alignment,
metadata addition, and page rounding are checked separately against 32-bit
overflow. Failure does not change existing allocations or heap ownership.

`kfree(NULL)` does nothing, including before initialization. Every other free
must use an original live heap pointer. Unaligned/interior pointers, foreign
VMM allocations, unknown addresses, and double frees cause a `Heap panic`.
The implementation searches its own block headers rather than dereferencing
an arbitrary argument's preceding bytes. This also diagnoses a stale pointer
to a completely unmapped extent without triggering a page fault during free.

As with ordinary allocators, a stale pointer cannot be distinguished from a
new allocation after the same address is reused. Do not use freed objects or
return heap backing through PMM/VMM directly. Invalid frees are programming
errors, not recoverable allocation failures.

## Extents, blocks, and alignment

An extent is one virtually contiguous VMM allocation. Its physical frames
may be scattered or above 256 MiB. Separate extents need not be adjacent in
virtual space, so growth does not depend on extending any existing mapping.

Each extent begins with a 16-byte header containing links, page count, and
live-object count. It is followed by blocks in address order:

```text
extent header | block header | payload | block header | payload | ...
   16 bytes      16 bytes                 16 bytes
```

Block headers store aligned payload capacity, free/allocated state, and links
to the previous/next block in that same extent. Their sizes are compile-time
checked. The initial extent is page-aligned, both headers are 16 bytes, and
every payload capacity is rounded to a multiple of 16. Splitting therefore
preserves 16-byte alignment for every subsequent payload.

The block list is an implicit free list: first-fit scans skip allocated blocks
and choose the first free block large enough. Searches are linear, deliberately
favoring a small readable implementation over a size-class or slab allocator.
Metadata stays outside each caller's requested payload, but the heap does not
provide red zones or protection against arbitrary kernel memory overwrites.

## Allocation and growth

For an existing free block, split only when the remainder can hold a new
header and a minimum 16-byte payload. Smaller tails are absorbed into the
allocated block so no unusable fragments become list entries.

If no block fits, request an extent from VMM. Small requests prefer four pages
(16 KiB) to amortize metadata and mapping costs; if that fails, retry the
minimum page count so the last one to three available pages remain useful.
Large requests use the minimum page count directly. Include both initial
headers when rounding up to pages.

Only after VMM has mapped and cleared all pages does the heap initialize and
link the new extent. VMM already rolls back partial physical exhaustion;
failed growth publishes no heap metadata and changes no heap counters.
`kmalloc` does not promise zeroing even though fresh VMM extents start cleared:
a reused block can contain data. `kcalloc` explicitly clears the requested
payload after a successful allocation, including recycled blocks.

## Freeing and page reclamation

Before accepting a pointer, walk the containing extent's known block headers
and validate sizes, links, and boundaries. An allocated block becomes free
and merges with a free right neighbor, then a free left neighbor. Removing
a neighbor's header adds those bytes to the merged payload capacity. Never
merge across extents, even if their virtual addresses happen to touch.

When an extent's live-object count reaches zero, all blocks must have merged
back into one whole free run. Unlink the extent and update counters before
calling `vmm_free_pages()`: the extent header itself becomes unmapped, so it
cannot be read afterward. An empty heap retains no pages or permanent arena.
Freeing an object inside a still-live extent does not unmap its individual
payload; only complete extent reclamation revokes the pages.

## Execution context

Heap operations inherit VMM's execution model: one CPU, no preemptive scheduler,
and no calls from IRQ/exception handlers. They preserve the caller's IF and
do not mask interrupts around scans or clearing. Individual PMM/page-table
operations retain their short interrupt guards. Introduce synchronization
before adding scheduling, SMP, or another execution context that uses the heap.

```c
uint32_t *values = kcalloc(100, sizeof(*values));
if (values != 0) {
    values[17] = 42;
    kfree(values);
}
```

There is no `krealloc`, DMA/address-constrained allocation, general aligned
allocation beyond 16 bytes, or object cache yet. Use VMM when an interface
requires whole page-aligned virtual allocations rather than heap objects.
