#ifndef XNF_PAGING_H
#define XNF_PAGING_H

#include "types.h"

/* Page-directory physical address; zero until paging_prepare runs. */
extern uint32_t paging_directory_physical;

void paging_prepare(void);
void paging_enable(void);

#endif
