# Experimental kernel threads

XNF has an opt-in, single-CPU ring-0 scheduler. Normal boot does not start it:
the ordinary bootstrap context continues its timer-driven idle loop. The
disposable QEMU tests start the scheduler after all memory initialization.
This is a context-switching milestone, not yet a general threading API.

## Startup and lifetime

`scheduler_start(first, second)` in [inc/scheduler.h](../inc/scheduler.h) creates
two permanent threads in addition to the calling bootstrap context. It requires
initialized VMM, valid entry functions, and normal single-CPU context. It masks
interrupts during setup, allocates each 16 KiB stack through VMM, and restores
the caller's original IF. If either allocation fails it rolls back the first;
invalid arguments or repeated startup also return false. Scheduling becomes
active only after both stacks and frames are ready. New threads inherit the
caller's IF; pass through startup with IF enabled for timer preemption.

The bootstrap context stays runnable and retains its existing stack. All three
contexts share the same address space and have equal priority. Entry functions
must not return; a return panics. There is no stop, thread creation after
startup, blocking, sleeping, idle policy, reclamation, or stack guard page.
The allocated stacks therefore remain owned for the kernel's lifetime.

## Switching

The PIT interrupts approximately every 10 ms. Each IRQ chooses the next of the
three contexts in round-robin order; `scheduler_yield()` uses a ring-0 software
interrupt to make the same selection explicitly. Before startup both paths
resume their original frame. The interrupt gates clear IF during selection,
and the timer acknowledges the PIC before switching stacks and returning.

The saved frame consists of PUSHAD's eight 32-bit words followed by the CPU's
same-privilege EIP, CS, and EFLAGS. The scheduler stores its stack pointer and
assembly restores the chosen frame with POPAD and IRETD. The artificial frame
on a new stack enters an assembly trampoline, which aligns the C call and
invokes the thread's entry. IRETD restores the interrupted IF and DF, while
the interrupt stubs clear DF before C. The flat ring-0 segment registers and
address space are shared. Floating-point, MMX and SIMD state are not saved;
thread code must not use them. TSS, privilege transitions, user stacks, and
per-process address spaces are outside this milestone.

## Execution restrictions

The scheduler does not synchronize the console, VMM, heap, helpers, object
manager, or caller-owned data. The VMM, heap and object manager require
non-preemptive normal context, and existing small PMM/page-table IRQ guards do
not protect a whole higher-level transaction. Test workers consequently do
not use these services. A caller may exclude maskable interrupts for an entire
operation on this one CPU, but long operations then delay timer delivery;
this is not a general synchronization design. Never yield while a critical
section or other non-preemptive service is in progress. Exceptions remain
fatal and can interrupt critical sections. SMP is not supported.

Future work must add appropriate preemption exclusion and service-level
synchronization, including console/panic behavior and object lifetime rules,
before arbitrary kernel threads can use shared services. Sleeping, an idle
thread, and safe thread termination can then build on those contracts.

## Validation

`python tests/scheduler_qemu.py --build cmake-build-tests` runs independent
128/512 MiB guests. Cooperative workers repeatedly switch with IF disabled and
check register/flag and stack preservation. Separate CPU-bound workers never
yield; the host verifies that both keep progressing as timer ticks advance,
checks their separate stacks and control registers, and inspects QEMU logs for
faults and resets. A separate memory-pressure guest forces the second stack
allocation to fail, verifies rollback, and retries successfully. Expected test
artifacts remain in the build directory.
