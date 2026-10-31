/*
 * Single-CPU preemption guards for multi-step normal-context transactions.
 * Brief IRQ masking protects nesting; the guarded operation may receive IRQs.
 */
#include "preempt.h"
#include "irq.h"
#include "panic.h"
#include "scheduler.h"

/* Scheduler state is inspected only while IRQs are masked. */
extern bool scheduler_take_pending(void);

static uint32_t depth;

void preempt_disable(void)
{
    uint32_t flags = irq_save();
    if (depth == ~(uint32_t)0) kernel_panic("Preempt", "nesting overflow");
    ++depth;
    irq_restore(flags);
}

void preempt_enable(void)
{
    uint32_t flags = irq_save();
    bool reschedule = false;
    if (!depth) kernel_panic("Preempt", "unbalanced enable");
    --depth;
    if (!depth && (flags & 0x200u)) reschedule = scheduler_take_pending();
    irq_restore(flags);
    /* Do not switch with IF deliberately disabled. A later timer will consume
       the pending request after the caller reenables interrupts. */
    if (reschedule) scheduler_yield();
}

bool preempt_disabled(void) { return depth != 0; }
