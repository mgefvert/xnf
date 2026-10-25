/*
 * Byte-granularity kernel heap backed by VMM page allocations.
 * Owns allocation blocks inside independent extents; callers only own payloads.
 */

#ifndef XNF_KHEAP_H
#define XNF_KHEAP_H

#include "types.h"

#define KHEAP_ALIGNMENT 16u

/* Initialize once after VMM. Normal, non-preemptive execution on one CPU only:
   never allocate/free from IRQ or exception handlers. Calls preserve IF. */
void kheap_init(void);

/* Return a 16-byte-aligned payload, with unspecified initial contents.
   Zero size, arithmetic overflow, use before initialization, or exhaustion
   returns NULL. Allocation failure leaves existing allocations intact. */
void *kmalloc(uint32_t size);

/* Like kmalloc, but clear exactly count * size bytes. Reject multiplication
   overflow and either zero argument, including before calling kmalloc. */
void *kcalloc(uint32_t count, uint32_t size);

/* NULL is a no-op. Every other argument must be an original, live heap
   payload pointer; invalid/interior/double frees panic. Do not pass heap
   pointers to VMM/PMM freeing functions or use them after kfree. */
void kfree(void *pointer);

/* Diagnostic counts: mapped VMM pages owned by the heap and live payloads.
   An empty heap owns no pages; completely free extents are returned to VMM. */
uint32_t kheap_allocated_pages(void);
uint32_t kheap_live_allocations(void);

#endif
