/*
 * Small first-fit kernel heap with splitting, coalescing, and extent reclamation.
 * VMM supplies mapped pages; this layer divides them into aligned byte payloads.
 */

#include "kheap.h"
#include "vmm.h"
#include "panic.h"
#include "conio.h"

#define DEFAULT_EXTENT_PAGES 4u
#define UINT32_MAX_VALUE (~(uint32_t)0)

typedef struct HeapBlock {
    uint32_t capacity;           /* Aligned payload bytes, excluding this header. */
    uint32_t free;
    struct HeapBlock *previous;  /* Neighbors in address order, within one extent. */
    struct HeapBlock *next;
} HeapBlock;

typedef struct HeapExtent {
    struct HeapExtent *previous; /* Extents need not be virtually adjacent. */
    struct HeapExtent *next;
    uint32_t pages;
    uint32_t live_blocks;
} HeapExtent;

/* Page-aligned extent + 16-byte header + 16-byte block header yields an aligned
   first payload. Every split uses an aligned capacity, preserving that rule. */
_Static_assert(sizeof(HeapBlock) == KHEAP_ALIGNMENT, "heap block alignment");
_Static_assert(sizeof(HeapExtent) == KHEAP_ALIGNMENT, "heap extent alignment");

static HeapExtent *first_extent;
static uint32_t heap_pages;
static uint32_t heap_allocations;
static bool heap_initialized;

static HeapBlock *first_block(HeapExtent *extent)
{
    return (HeapBlock *)(extent + 1);
}

/* Metadata lives in heap-owned mapped memory, never in the caller's payload.
   Check geometry before following a next pointer, so a damaged size/link is
   diagnosed as corruption instead of silently coalescing unrelated memory. */
static void validate_block(HeapExtent *extent, HeapBlock *block, HeapBlock *previous)
{
    uint32_t address = (uint32_t)block;
    uint32_t end = (uint32_t)extent + extent->pages * PAGE_SIZE;
    uint32_t next;
    if (address < (uint32_t)first_block(extent) || address > end - sizeof(HeapBlock) ||
        (address & (KHEAP_ALIGNMENT - 1u)) != 0) {
        kernel_panic("Heap", "corrupt block address");
    }
    if (block->previous != previous || block->free > 1u ||
        block->capacity < KHEAP_ALIGNMENT ||
        (block->capacity & (KHEAP_ALIGNMENT - 1u)) != 0 ||
        block->capacity > end - address - sizeof(HeapBlock)) {
        kernel_panic("Heap", "corrupt block metadata");
    }
    next = address + sizeof(HeapBlock) + block->capacity;
    if ((next == end && block->next != 0) ||
        (next != end && (next > end - sizeof(HeapBlock) - KHEAP_ALIGNMENT ||
                        block->next != (HeapBlock *)next))) {
        kernel_panic("Heap", "corrupt block boundary");
    }
}

/* Growth rounds only after all additions have been checked for overflow.
   Small requests prefer four pages to amortize metadata, but retry their
   minimum size under pressure. Large requests need no speculative spare RAM. */
static HeapExtent *grow_heap(uint32_t capacity)
{
    uint32_t overhead = sizeof(HeapExtent) + sizeof(HeapBlock);
    uint32_t pages;
    uint32_t minimum_pages;
    HeapExtent *extent;
    HeapBlock *block;
    if (capacity > UINT32_MAX_VALUE - overhead - (PAGE_SIZE - 1u)) {
        return 0;
    }
    minimum_pages = (capacity + overhead + PAGE_SIZE - 1u) / PAGE_SIZE;
    pages = minimum_pages;
    if (pages < DEFAULT_EXTENT_PAGES) {
        pages = DEFAULT_EXTENT_PAGES;
    }
    extent = vmm_alloc_pages(pages);
    if (extent == 0 && pages != minimum_pages) {
        pages = minimum_pages;
        extent = vmm_alloc_pages(pages);
    }
    if (extent == 0) {
        return 0;
    }
    /* VMM has completed mapping/zeroing. Initialize this private extent before
       publishing it in the list; failure above changed no heap metadata. */
    extent->previous = 0;
    extent->next = first_extent;
    extent->pages = pages;
    extent->live_blocks = 0;
    block = first_block(extent);
    block->capacity = pages * PAGE_SIZE - overhead;
    block->free = 1;
    block->previous = 0;
    block->next = 0;
    if (first_extent != 0) {
        first_extent->previous = extent;
    }
    first_extent = extent;
    heap_pages += pages;
    return extent;
}

static void *claim_block(HeapExtent *extent, HeapBlock *block, uint32_t capacity)
{
    /* A remainder is useful only if it holds another header and one minimum
       payload. Otherwise absorb the small tail into this allocation. */
    if (block->capacity - capacity >= sizeof(HeapBlock) + KHEAP_ALIGNMENT) {
        HeapBlock *tail = (HeapBlock *)((uint8_t *)(block + 1) + capacity);
        tail->capacity = block->capacity - capacity - sizeof(HeapBlock);
        tail->free = 1;
        tail->previous = block;
        tail->next = block->next;
        if (tail->next != 0) {
            tail->next->previous = tail;
        }
        block->capacity = capacity;
        block->next = tail;
    }
    block->free = 0;
    ++extent->live_blocks;
    ++heap_allocations;
    return block + 1;
}

void kheap_init(void)
{
    if (heap_initialized || !vmm_is_initialized()) {
        kernel_panic("Heap", "initialize once after VMM");
    }
    /* No pages are needed merely to create an empty heap. This keeps boot
       viable even for ownership-test E820 fixtures with fictitious high RAM. */
    heap_initialized = true;
    kputs("Heap ready.\n");
}

bool kheap_is_initialized(void)
{
    return heap_initialized;
}

void *kmalloc(uint32_t size)
{
    HeapExtent *extent;
    uint32_t capacity;
    if (!heap_initialized || size == 0 || size > UINT32_MAX_VALUE - (KHEAP_ALIGNMENT - 1u)) {
        return 0;
    }
    /* Round up by clearing four low bits after adding 15. This gives every
       payload the alignment required by current kernel scalar/object types. */
    capacity = (size + KHEAP_ALIGNMENT - 1u) & ~(KHEAP_ALIGNMENT - 1u);
    for (extent = first_extent; extent != 0; extent = extent->next) {
        HeapBlock *block;
        HeapBlock *previous = 0;
        for (block = first_block(extent); block != 0; block = block->next) {
            validate_block(extent, block, previous);
            if (block->free && block->capacity >= capacity) {
                return claim_block(extent, block, capacity);
            }
            previous = block;
        }
    }
    extent = grow_heap(capacity);
    return extent == 0 ? 0 : claim_block(extent, first_block(extent), capacity);
}

void *kcalloc(uint32_t count, uint32_t size)
{
    uint32_t bytes;
    uint32_t i;
    volatile uint8_t *payload;
    if (count == 0 || size == 0 || size > UINT32_MAX_VALUE / count) {
        return 0;
    }
    bytes = count * size;
    payload = kmalloc(bytes);
    if (payload != 0) {
        /* A reused free block can contain old data even though newly grown
           VMM extents start zeroed. Only calloc promises cleared payloads. */
        for (i = 0; i < bytes; ++i) {
            payload[i] = 0;
        }
    }
    return (void *)payload;
}

static void merge_right(HeapExtent *extent, HeapBlock *block)
{
    HeapBlock *right = block->next;
    if (right == 0) {
        return;
    }
    validate_block(extent, right, block);
    if (right->free) {
        /* Removing the neighbor's header makes those bytes usable payload.
           Never merge across extent boundaries, even if VMM placed them nearby. */
        block->capacity += sizeof(HeapBlock) + right->capacity;
        block->next = right->next;
        if (block->next != 0) {
            block->next->previous = block;
        }
    }
}

void kfree(void *pointer)
{
    HeapExtent *extent;
    uint32_t address = (uint32_t)pointer;
    if (pointer == 0) {
        return;
    }
    if (!heap_initialized || (address & (KHEAP_ALIGNMENT - 1u)) != 0) {
        kernel_panic("Heap", "invalid or already freed pointer");
    }
    for (extent = first_extent; extent != 0; extent = extent->next) {
        HeapBlock *block;
        HeapBlock *previous = 0;
        uint32_t end = (uint32_t)extent + extent->pages * PAGE_SIZE;
        if (address < (uint32_t)first_block(extent) + sizeof(HeapBlock) || address >= end) {
            continue;
        }
        /* Never read pointer[-header] from an untrusted argument. Walk our
           known headers and accept only an exact live payload base. */
        for (block = first_block(extent); block != 0; block = block->next) {
            validate_block(extent, block, previous);
            if ((void *)(block + 1) != pointer) {
                previous = block;
                continue;
            }
            if (block->free || extent->live_blocks == 0 || heap_allocations == 0) {
                kernel_panic("Heap", "invalid or already freed pointer");
            }
            block->free = 1;
            --extent->live_blocks;
            --heap_allocations;
            merge_right(extent, block);
            if (previous != 0 && previous->free) {
                merge_right(extent, previous);
                block = previous;
            }
            if (extent->live_blocks == 0) {
                uint32_t pages = extent->pages;
                /* All free blocks must have coalesced back to one whole run. */
                if (block != first_block(extent) || block->next != 0 ||
                    block->capacity != pages * PAGE_SIZE - sizeof(HeapExtent) - sizeof(HeapBlock)) {
                    kernel_panic("Heap", "empty extent did not coalesce");
                }
                if (extent->previous != 0) {
                    extent->previous->next = extent->next;
                } else {
                    first_extent = extent->next;
                }
                if (extent->next != 0) {
                    extent->next->previous = extent->previous;
                }
                heap_pages -= pages;
                /* Unlink before returning pages: extent and its block headers
                   become unmapped, so do not read them after this call. */
                if (!vmm_free_pages(extent)) {
                    kernel_panic("Heap", "VMM rejected an owned extent");
                }
            }
            return;
        }
        break;
    }
    kernel_panic("Heap", "invalid or already freed pointer");
}

uint32_t kheap_allocated_pages(void)
{
    return heap_pages;
}

uint32_t kheap_live_allocations(void)
{
    return heap_allocations;
}
