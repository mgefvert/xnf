#include "vmm.h"
#include "paging_internal.h"
#include "pmm.h"
#include "conio.h"
#include "panic.h"

#define PAGE_COUNT (KERNEL_VIRTUAL_END / PAGE_SIZE)
#define BITMAP_WORDS (PAGE_COUNT / 32u)

/* Three bits per virtual page: 24 KiB total, inside the linker reservation.
   Allocation boundaries distinguish exact bases from interior pointers. */
static uint32_t occupied[BITMAP_WORDS];
static uint32_t heads[BITMAP_WORDS];
static uint32_t tails[BITMAP_WORDS];
static uint32_t first_page;
static uint32_t free_virtual_pages;
static bool initialized;

static bool bit(const uint32_t *map, uint32_t page)
{
    return (map[page / 32u] & (1u << (page % 32u))) != 0;
}

static void mark(uint32_t *map, uint32_t page, bool value)
{
    uint32_t mask = 1u << (page % 32u);
    if (value) {
        map[page / 32u] |= mask;
    } else {
        map[page / 32u] &= ~mask;
    }
}

void vmm_init(void)
{
    uint32_t page;
    PagingMapping mapping;
    if (initialized || !pmm_is_initialized() || paging_identity_end == 0 ||
        paging_identity_end >= KERNEL_VIRTUAL_END ||
        !paging_query((uint32_t)&occupied[0], &mapping) ||
        mapping.physical_address != (uint32_t)&occupied[0]) {
        kernel_panic("VMM", "invalid initialization order or bootstrap map");
    }
    first_page = paging_identity_end / PAGE_SIZE;
    for (page = 0; page < PAGE_COUNT; ++page) {
        if (page >= first_page && paging_query(page * PAGE_SIZE, &mapping)) {
            kernel_panic("VMM", "dynamic arena already mapped");
        }
        mark(occupied, page, page < first_page);
    }
    free_virtual_pages = PAGE_COUNT - first_page;
    initialized = true;
    kprintf("VMM ready: arena [%x, %x), free pages=%d\n",
            paging_identity_end, KERNEL_VIRTUAL_END, free_virtual_pages);
}

/* Unmap before returning physical ownership. Only committed or private
   in-progress VMM pages enter here; a missing mapping is an invariant failure. */
static void release_page(uint32_t page)
{
    PagingMapping mapping;
    uint32_t address = page * PAGE_SIZE;
    if (!paging_query(address, &mapping) || !paging_unmap(address) ||
        !pmm_free_page(mapping.physical_address)) {
        kernel_panic("VMM", "lost mapping or physical ownership");
    }
}

void *vmm_alloc_pages(uint32_t page_count)
{
    uint32_t page;
    uint32_t run = 0;
    uint32_t start = 0;
    uint32_t mapped = 0;

    if (!initialized || page_count == 0 || page_count > free_virtual_pages) {
        return 0;
    }
    for (page = first_page; page < PAGE_COUNT; ++page) {
        run = bit(occupied, page) ? 0 : run + 1u;
        if (run == page_count) {
            start = page + 1u - page_count;
            break;
        }
    }
    if (run != page_count) {
        return 0;
    }
    /* Only normal context may allocate: IRQ handlers never enter VMM and
       there is no preemptive scheduler or second CPU. The reserved run is
       private until publication; zeroing does not need to mask interrupts. */
    for (page = 0; page < page_count; ++page) {
        mark(occupied, start + page, true);
    }
    while (mapped < page_count) {
        uint32_t physical = pmm_alloc_page();
        uint32_t address = (start + mapped) * PAGE_SIZE;
        volatile uint32_t *destination;
        uint32_t word;
        if (physical == 0) {
            break;
        }
        if (!paging_map(address, physical, PAGING_WRITABLE)) {
            if (!pmm_free_page(physical)) {
                kernel_panic("VMM", "rollback lost an unmapped physical frame");
            }
            /* An occupied PTE in a free VMM slot indicates corruption. */
            kernel_panic("VMM", "mapping collided with virtual ownership");
        }
        ++mapped;
        destination = (volatile uint32_t *)address;
        for (word = 0; word < PAGE_SIZE / sizeof(uint32_t); ++word) {
            destination[word] = 0;
        }
    }
    if (mapped != page_count) {
        while (mapped != 0) {
            release_page(start + --mapped);
        }
        for (page = 0; page < page_count; ++page) {
            mark(occupied, start + page, false);
        }
        return 0;
    }
    mark(heads, start, true);
    mark(tails, start + page_count - 1u, true);
    free_virtual_pages -= page_count;
    return (void *)(start * PAGE_SIZE);
}

bool vmm_free_pages(void *base)
{
    uint32_t address = (uint32_t)base;
    uint32_t start = address / PAGE_SIZE;
    uint32_t end;
    uint32_t page;

    if (!initialized || address < paging_identity_end || address >= KERNEL_VIRTUAL_END ||
        (address & (PAGE_SIZE - 1u)) != 0 || !bit(heads, start)) {
        return false;
    }
    /* Validate the whole allocation before changing anything. A damaged
       boundary or absent mapping is corruption, not an invalid caller base. */
    for (end = start; end < PAGE_COUNT; ++end) {
        PagingMapping mapping;
        if (!bit(occupied, end) || (end != start && bit(heads, end)) ||
            !paging_query(end * PAGE_SIZE, &mapping)) {
            kernel_panic("VMM", "allocation metadata does not match mappings");
        }
        if (bit(tails, end)) {
            break;
        }
    }
    if (end == PAGE_COUNT) {
        kernel_panic("VMM", "allocation has no end");
    }
    for (page = start; page <= end; ++page) {
        release_page(page);
        mark(occupied, page, false);
    }
    mark(heads, start, false);
    mark(tails, end, false);
    free_virtual_pages += end - start + 1u;
    return true;
}

uint32_t vmm_free_page_count(void)
{
    return free_virtual_pages;
}
