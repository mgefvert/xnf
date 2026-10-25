/*
 * Paging startup and read-only translation queries for kernel clients.
 * Mapping mutations are kept in paging_internal.h so allocation ownership stays in VMM.
 */

#ifndef XNF_PAGING_H
#define XNF_PAGING_H

#include <stdbool.h>
#include "types.h"
#include "mm.h"

#define PAGING_WRITABLE 0x002u

typedef struct PagingMapping {
    uint32_t physical_address; /* Includes the queried byte offset. */
    uint32_t flags;            /* Effective permissions; currently RW only. */
} PagingMapping;

/* Page-directory physical address; zero until paging_prepare runs. */
extern uint32_t paging_directory_physical;
/* Exclusive end of the permanently occupied identity-mapped prefix. */
extern uint32_t paging_identity_end;

void paging_prepare(void);
/* Must follow preparation immediately; activation must allocate/write no tables. */
void paging_enable(void);
/* On success, return the physical byte address and RW permission allowed by
   both paging levels. On failure, leave the caller's output object unchanged. */
bool paging_query(uint32_t virtual_address, PagingMapping *mapping);

#endif
