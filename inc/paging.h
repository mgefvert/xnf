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
void paging_enable(void);
bool paging_query(uint32_t virtual_address, PagingMapping *mapping);

#endif
