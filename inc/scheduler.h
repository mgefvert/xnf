/*
 * Experimental single-CPU, ring-0 scheduler for two permanent kernel threads.
 * Shared kernel services use single-CPU preemption exclusion in normal context.
 */
#ifndef XNF_SCHEDULER_H
#define XNF_SCHEDULER_H

#include <stdbool.h>
#include "types.h"

typedef void (*KernelThreadEntry)(void);

/* Start once in normal context after VMM initialization. Creates both stacks
   before enabling scheduling; failure leaves scheduling inactive. Both entries
   inherit the caller's IF and must never return. The bootstrap caller becomes
   the third runnable context. */
bool scheduler_start(KernelThreadEntry first, KernelThreadEntry second);

/* Switch to the next runnable context. Never yield within a critical section. */
void scheduler_yield(void);

/* Sleep for 1..0x7fffffff ticks. Requires active scheduling, enabled IRQs,
   and normal context outside preemption guards. Returns false on invalid use. */
bool scheduler_sleep(uint32_t ticks);

/* Permanently remove the bootstrap context from scheduling and run the other
   threads. Requires active scheduling, enabled IRQs, and no preemption guard. */
void scheduler_finish_bootstrap(void) __attribute__((noreturn));

#endif
