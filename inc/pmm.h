#ifndef XNF_PMM_H
#define XNF_PMM_H

#include <stdbool.h>
#include "types.h"

#define PMM_PAGE_SIZE 4096u
#define PMM_LOW_LIMIT 0x10000000u

/* Initialize once from memblock's normalized E820 map and boot reservations,
   then permanently retire bootstrap allocations before publishing ownership. */
void pmm_init(void);

/* Physical addresses, never virtual pointers; zero means failure. General
   allocation prefers high RAM to preserve low frames. Even a low frame must
   have its current mapping checked before dereferencing it at this stage. */
uint32_t pmm_alloc_page(void);
uint32_t pmm_alloc_page_low(void);
bool pmm_free_page(uint32_t physical_address);

/* Managed pages exclude permanent reservations; free pages exclude PMM loans. */
uint32_t pmm_total_pages(void);
uint32_t pmm_free_pages(void);
void pmm_self_test(void);

#endif
