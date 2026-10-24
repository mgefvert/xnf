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
    uint64_t base;
    uint64_t length;
    MemblockType type;
} MemblockRegion;

void display_e820_map(void);
void memblock_init(void);
uint64_t memblock_alloc(uint64_t size, uint64_t alignment);
/* Called by pmm_init after importing reservations, with IRQs disabled.
   Permanently forbid new allocations; existing regions remain readable. */
void memblock_retire(void);
uint32_t memblock_region_count(void);
const MemblockRegion *memblock_region(uint32_t index);
void memblock_dump(void);

#endif
