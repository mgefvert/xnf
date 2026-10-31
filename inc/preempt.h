/*
 * Nestable single-CPU exclusion of thread switches in normal kernel context.
 * Timer IRQs remain enabled when the caller enabled them; this is not an SMP lock.
 */
#ifndef XNF_PREEMPT_H
#define XNF_PREEMPT_H

#include <stdbool.h>

/* Calls preserve IF. Never sleep or yield inside a preemption-disabled region. */
void preempt_disable(void);
void preempt_enable(void);
/* Used by the scheduler with IRQs masked; not a general ownership query. */
bool preempt_disabled(void);

#endif
