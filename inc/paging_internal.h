/*
 * Internal page-table editing interface used by VMM and its tests.
 * Changing a translation does not allocate or release the physical frame behind it.
 */

#ifndef XNF_PAGING_INTERNAL_H
#define XNF_PAGING_INTERNAL_H

#include "paging.h"

/* VMM implementation interface, not an allocation API. Caller owns/reserves
   the virtual slot in non-preemptive normal context. Each operation preserves
   IF and masks IRQs only around its PTE edit and TLB invalidation. These
   operations never acquire or release physical ownership. Only dynamic kernel
   pages can be changed. Flags are zero (read-only) or PAGING_WRITABLE.
   CR0.WP enforces read-only mappings even for supervisor writes. */
bool paging_map(uint32_t virtual_address, uint32_t physical_address, uint32_t flags);
bool paging_unmap(uint32_t virtual_address);
bool paging_protect(uint32_t virtual_address, uint32_t flags);

#endif
