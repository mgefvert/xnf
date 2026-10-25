/*
 * Bootstrap physical-region map and reservation interface.
 * Regions retain byte precision; PMM later imports only complete available pages.
 */

#ifndef XNF_MEMORY_H
#define XNF_MEMORY_H

#include "types.h"

typedef enum {
    MEMBLOCK_UNAVAILABLE,
    MEMBLOCK_AVAILABLE,
    MEMBLOCK_RESERVED,
    MEMBLOCK_KERNEL,
    MEMBLOCK_BOOT_ALLOCATED,
    MEMBLOCK_ACPI_RECLAIMABLE,
    MEMBLOCK_ACPI_NVS
} MemblockType;

typedef struct {
    /* Half-open physical range [base, base + length), before page rounding.
       64-bit fields preserve firmware reports even above addressable RAM. */
    uint64_t base;
    uint64_t length;
    MemblockType type;
} MemblockRegion;

void display_e820_map(void);
void memblock_init(void);
/* Before paging/PMM only: reserve aligned bytes, return a physical base or 0.
   Allocation is permanent and does not initialize its memory contents. */
uint64_t memblock_alloc(uint64_t size, uint64_t alignment);
/* Called by pmm_init after importing reservations, with IRQs disabled.
   Permanently forbid new allocations; existing regions remain readable. */
void memblock_retire(void);
uint32_t memblock_region_count(void);
/* Borrow a read-only region pointer; callers must not alter ownership metadata. */
const MemblockRegion *memblock_region(uint32_t index);
void memblock_dump(void);

#endif
