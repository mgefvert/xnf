/*
 * Save and restore interrupt state around short single-CPU critical sections.
 * These helpers exclude maskable IRQs; they are not SMP locks or exception guards.
 */

#ifndef XNF_IRQ_H
#define XNF_IRQ_H

#include "types.h"

/* Single-CPU critical sections; nesting preserves the caller's IF. */
static inline uint32_t irq_save(void)
{
    uint32_t flags;
    /* Save EFLAGS before CLI changes IF (interrupt-enable bit 9). The memory
       clobber prevents compiler movement of shared accesses across the guard. */
    __asm__ volatile ("pushfl; popl %0; cli" : "=r" (flags) : : "memory");
    return flags;
}

static inline void irq_restore(uint32_t flags)
{
    /* Restore the saved state rather than using STI: a nested guard must not
       enable interrupts that its caller deliberately left disabled. */
    __asm__ volatile ("pushl %0; popfl" : : "r" (flags) : "memory", "cc");
}

#endif
