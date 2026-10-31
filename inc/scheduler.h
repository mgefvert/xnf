/*
 * Experimental single-CPU, ring-0 scheduler for two permanent kernel threads.
 * Shared kernel services remain non-preemptive and require external exclusion.
 */
#ifndef XNF_SCHEDULER_H
#define XNF_SCHEDULER_H

#include <stdbool.h>

typedef void (*KernelThreadEntry)(void);

/* Start once in normal context after VMM initialization. Creates both stacks
   before enabling scheduling; failure leaves scheduling inactive. Both entries
   inherit the caller's IF and must never return. The bootstrap caller becomes
   the third runnable context. No allocation is allowed after scheduling starts
   unless the caller excludes preemption for the entire operation. */
bool scheduler_start(KernelThreadEntry first, KernelThreadEntry second);

/* Switch to the next runnable context. Never yield within a critical section. */
void scheduler_yield(void);

#endif
