#ifndef XNF_IRQ_H
#define XNF_IRQ_H

#include "types.h"

/* Single-CPU critical sections; nesting preserves the caller's IF. */
static inline uint32_t irq_save(void)
{
    uint32_t flags;
    __asm__ volatile ("pushfl; popl %0; cli" : "=r" (flags) : : "memory");
    return flags;
}

static inline void irq_restore(uint32_t flags)
{
    __asm__ volatile ("pushl %0; popfl" : : "r" (flags) : "memory", "cc");
}

#endif
