/*
 * Kernel virtual-page allocation interface for normal execution context.
 * Returns usable zeroed pointers while hiding the placement of physical backing frames.
 */

#ifndef XNF_VMM_H
#define XNF_VMM_H

#include "paging.h"

/* Non-preemptive normal context only, on one CPU. Never call
   from IRQ/exception handlers. IRQs may run during zeroing and scans; calls
   preserve IF. Initialize once after PMM handover. */
void vmm_init(void);
bool vmm_is_initialized(void);

/* Zeroed, writable, supervisor-only, virtually contiguous 4 KiB pages.
   Physical frames need not be contiguous. Zero/oversized requests, use before
   initialization, or exhaustion return NULL without changing ownership. */
void *vmm_alloc_pages(uint32_t page_count);

/* Only an original live allocation base is accepted. NULL, interior/unaligned
   pointers, permanent mappings, and double frees return false. Do not free
   backing frames through PMM or alter their mappings behind the VMM. */
bool vmm_free_pages(void *base);
uint32_t vmm_free_page_count(void);

#endif
