# Experimental kernel threads

XNF has a single-CPU ring-0 scheduler. Normal boot starts two demonstration
threads after memory and object initialization. One prints `A` and the other
prints `B`, each then sleeping for 100 ticks, approximately one second. Output
order is not guaranteed. Test builds instead retain the bootstrap idle loop so
host-selected allocator and scheduler fixtures can start from a clean state.
The scheduler provides basic preemption and timed sleep, not yet a general
threading API.

## Startup and lifetime

`scheduler_start(first, second)` in [inc/scheduler.h](../inc/scheduler.h) creates
two permanent threads in addition to the calling bootstrap context. It requires
initialized VMM, valid entry functions, and normal single-CPU context. It masks
interrupts during setup, allocates each 16 KiB stack through VMM, and restores
the caller's original IF. If either allocation fails it rolls back the first;
invalid arguments or repeated startup also return false. Scheduling becomes
active only after both stacks and frames are ready. New threads inherit the
caller's IF; pass through startup with IF enabled for timer preemption.

The bootstrap context retains its existing stack and can also sleep. Normal
boot calls `scheduler_finish_bootstrap()` after startup, permanently removing
that context from scheduling. It requires enabled IRQs and no preemption guard;
it does not return. The existing assembly halt is a fallback if `kernel_main`
ever returns. All ordinary contexts share the same address space and have
equal priority. A separate idle context uses a 4 KiB stack inside the validated
kernel extent. Entry functions
must not return; a return panics. There is no stop, thread creation after
startup, general wait primitive, reclamation, or stack guard page. The two
allocated stacks therefore remain owned for the kernel's lifetime.

## Switching

The PIT interrupts approximately every 10 ms. Each IRQ chooses the next of the
three runnable contexts in round-robin order; `scheduler_yield()` uses a
ring-0 software interrupt to make the same selection explicitly. Before
startup both paths resume their original frame. The interrupt gates clear IF
during selection, and the timer acknowledges the PIC before switching stacks
and returning. If preemption is disabled, the interrupted thread continues and
the switch is deferred until the outermost guard is released. If that release
occurs with IF disabled, the next interrupt after IF is restored reconsiders it.

`scheduler_sleep(ticks)` sleeps the current thread for at least the requested
number of PIT ticks. It accepts `1..0x7fffffff` ticks in normal context with
IRQs enabled, scheduling active, and no preemption guard held; otherwise it
returns false without sleeping. It masks IRQs while recording the deadline
and switching, so a timer cannot be lost between the check and sleep. Wakeup
uses modular tick arithmetic and works across 32-bit wrap. Deadlines must be
within half the tick range; the comparison assumes a sleeper is examined
within half a counter cycle of its deadline.

If every remaining ordinary context sleeps, the scheduler runs the idle
context. Its `STI; HLT` loop wakes on an IRQ, and the scheduler switches to a
ready context when its deadline arrives. Idle is not selected while an
ordinary context is ready.

The saved frame consists of PUSHAD's eight 32-bit words followed by the CPU's
same-privilege EIP, CS, and EFLAGS. The scheduler stores its stack pointer and
assembly restores the chosen frame with POPAD and IRETD. The artificial frame
on a new stack enters an assembly trampoline, which aligns the C call and
invokes the thread's entry. IRETD restores the interrupted IF and DF, while
the interrupt stubs clear DF before C. The flat ring-0 segment registers and
address space are shared. Floating-point, MMX and SIMD state are not saved;
thread code must not use them. TSS, privilege transitions, user stacks, and
per-process address spaces are outside this milestone.

## Preemption and shared services

`preempt_disable()` and `preempt_enable()` nest on this one CPU, preserving
the caller's IF. Short IRQ-masked sections protect the nesting counter; timer
IRQs still run while a guard is held if IF was enabled. The guard prevents
thread switches but does not exclude exceptions, IRQ handlers, or other CPUs.
Never sleep or yield while it is held. Unbalanced enable or nesting overflow is
fatal. This is not a spinlock, an SMP lock, or a general blocking primitive.

VMM, heap, console, and object-manager transactions use these guards internally.
They remain normal-context services; IRQ handlers must not allocate or use the
object manager. Console calls serialize whole operations, while fatal reports
never wait for an interrupted thread's console lock. The object manager drops
its guard before invoking a destructor. See [MEMORY.md](MEMORY.md),
[HEAP.md](HEAP.md), [IO.md](IO.md), and [OBJECTS.md](OBJECTS.md).
Caller-owned containers, object bodies, and multi-call transactions still need
explicit synchronization or exclusive ownership. SMP, safe thread termination,
and general waiting primitives remain future work.

## Validation

`python tests/scheduler_qemu.py --build cmake-build-tests` runs independent
128/512 MiB guests. Cooperative workers repeatedly switch with IF disabled and
check register/flag and stack preservation. Separate CPU-bound workers never
yield; the host verifies that both keep progressing as timer ticks advance,
checks their separate stacks and control registers, and inspects QEMU logs for
faults and resets. Further guests exercise nested guards with continuing ticks,
concurrent allocator/object/console work, destructors that sleep, tick wrap,
all threads sleeping, idle, and wakeup. A separate memory-pressure guest forces
the second stack allocation to fail, verifies rollback, and retries successfully.
Expected test artifacts remain in the build directory.

`python tests/ab_qemu.py --build cmake-build-debug` checks normal A/B startup
in 128/512 MiB guests, including continued output, timer progress, bootstrap
retirement, idle, control registers, and fault/reset logs. Running it against
`cmake-build-tests` redirects a disposable fixture guest into the same demo.
