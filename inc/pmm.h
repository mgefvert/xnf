#ifndef XNF_PMM_H
#define XNF_PMM_H

#include <stdbool.h>
#include "types.h"
#include "mm.h"

/* Initialize once from memblock's normalized E820 map and boot reservations,
   then permanently retire bootstrap allocations before publishing ownership. */
void pmm_init(void);
bool pmm_is_initialized(void);

/* Physical addresses, never virtual pointers; zero means failure. No address
   preference or mapping is promised. Never dereference an unmapped frame. */
uint32_t pmm_alloc_page(void);
bool pmm_free_page(uint32_t physical_address);

/* Managed pages exclude permanent reservations; free pages exclude PMM loans. */
uint32_t pmm_total_pages(void);
uint32_t pmm_free_pages(void);

#endif
