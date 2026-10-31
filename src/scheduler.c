/*
 * Minimal round-robin scheduler for a bootstrap context and two kernel threads.
 * Interrupt assembly owns frame entry/exit; all contexts share one address space.
 */
#include "scheduler.h"
#include "irq.h"
#include "vmm.h"
#include "panic.h"
#include "preempt.h"

#define THREAD_COUNT 3u
#define IDLE_THREAD THREAD_COUNT
#define STACK_PAGES 4u
#define STACK_BYTES (STACK_PAGES * 4096u)
#define INTERRUPT_FLAG 0x200u

/* Must match PUSHAD followed by the same-privilege CPU IRETD frame. POPAD
   ignores saved_esp; IRETD leaves ESP immediately above this structure. */
typedef struct ThreadFrame {
    uint32_t edi, esi, ebp, saved_esp, ebx, edx, ecx, eax;
    uint32_t eip, cs, eflags;
} ThreadFrame;
typedef char frame_size_check[sizeof(ThreadFrame) == 44 ? 1 : -1];
typedef char frame_eip_check[__builtin_offsetof(ThreadFrame, eip) == 32 ? 1 : -1];

typedef struct Thread {
    ThreadFrame *frame;
    KernelThreadEntry entry;
    uint32_t wake_tick;
    bool sleeping;
} Thread;

static Thread threads[THREAD_COUNT + 1u];
/* Idle executes only STI; HLT and interrupt handlers. This stack is part of
   the validated bootstrap extent and requires no extra VMM allocation. */
static uint32_t idle_stack[1024] __attribute__((aligned(16)));
static uint32_t current;
static bool active;
static bool reschedule_pending;
static bool bootstrap_finished;

extern void scheduler_thread_entry(void);
extern void interrupts_enable_and_halt(void);
extern volatile uint32_t timer_ticks;

static void idle_thread(void)
{
    for (;;) interrupts_enable_and_halt();
}

static void prepare_thread(Thread *thread, void *stack, KernelThreadEntry entry,
                           uint32_t flags, uint32_t stack_bytes)
{
    /* VMM and .bss supply zeroed stacks. Place the artificial frame at the
       high end so the first IRETD exposes an aligned, empty downward stack. */
    ThreadFrame *frame = (ThreadFrame *)((uint8_t *)stack + stack_bytes - sizeof(ThreadFrame));
    frame->eip = (uint32_t)scheduler_thread_entry;
    frame->cs = 0x08u;
    /* EFLAGS bit 1 is reserved and always set. New threads inherit only IF;
       in particular they begin with DF clear and do not inherit debug flags. */
    frame->eflags = 0x2u | (flags & INTERRUPT_FLAG);
    thread->frame = frame;
    thread->entry = entry;
}

bool scheduler_start(KernelThreadEntry first, KernelThreadEntry second)
{
    uint32_t flags = irq_save();
    void *first_stack;
    void *second_stack;
    if (active || !first || !second || !vmm_is_initialized()) {
        irq_restore(flags);
        return false;
    }
    /* No thread can run until both allocations and frames are complete. */
    first_stack = vmm_alloc_pages(STACK_PAGES);
    if (!first_stack) {
        irq_restore(flags);
        return false;
    }
    second_stack = vmm_alloc_pages(STACK_PAGES);
    if (!second_stack) {
        if (!vmm_free_pages(first_stack)) kernel_panic("Scheduler", "stack rollback failed");
        irq_restore(flags);
        return false;
    }
    prepare_thread(&threads[1], first_stack, first, flags, STACK_BYTES);
    prepare_thread(&threads[2], second_stack, second, flags, STACK_BYTES);
    prepare_thread(&threads[IDLE_THREAD], idle_stack, idle_thread, INTERRUPT_FLAG, sizeof(idle_stack));
    current = 0;
    active = true;
    irq_restore(flags);
    return true;
}

/* Called only from interrupt gates with IF clear. Before startup, return the
   same frame so ordinary boot and its disposable tests retain their behavior. */
ThreadFrame *scheduler_select(ThreadFrame *frame)
{
    uint32_t i, candidate, now;
    if (!active) return frame;
    if (preempt_disabled()) {
        reschedule_pending = true;
        return frame;
    }
    reschedule_pending = false;
    threads[current].frame = frame;
    now = timer_ticks;
    for (i = 0; i < THREAD_COUNT; ++i) {
        /* Deadlines at most half the counter range away make signed modular
           subtraction unambiguous even when the 32-bit tick counter wraps. */
        if (threads[i].sleeping && (int)(now - threads[i].wake_tick) >= 0)
            threads[i].sleeping = false;
    }
    candidate = current < THREAD_COUNT ? current : THREAD_COUNT - 1u;
    for (i = 0; i < THREAD_COUNT; ++i) {
        candidate = (candidate + 1u) % THREAD_COUNT;
        if (!threads[candidate].sleeping && (candidate != 0 || !bootstrap_finished)) {
            current = candidate;
            return threads[current].frame;
        }
    }
    current = IDLE_THREAD;
    return threads[current].frame;
}

/* Called by the outermost preemption-enable with IRQs masked. */
bool scheduler_take_pending(void)
{
    bool pending = active && reschedule_pending;
    reschedule_pending = false;
    return pending;
}

bool scheduler_sleep(uint32_t ticks)
{
    uint32_t flags = irq_save();
    if (!active || current == IDLE_THREAD || !ticks || ticks > 0x7FFFFFFFu ||
        !(flags & INTERRUPT_FLAG) || preempt_disabled()) {
        irq_restore(flags);
        return false;
    }
    /* Mark sleep and switch with IRQs masked so a tick cannot be lost between
       checking time and parking. IRETD resumes here with IF still clear. */
    threads[current].wake_tick = timer_ticks + ticks;
    threads[current].sleeping = true;
    scheduler_yield();
    irq_restore(flags);
    return true;
}

void scheduler_finish_bootstrap(void)
{
    uint32_t flags = irq_save();
    if (!active || current != 0 || !(flags & INTERRUPT_FLAG) || preempt_disabled())
        kernel_panic("Scheduler", "invalid bootstrap finish");
    /* Once marked, the bootstrap frame must never be selected again. Keeping
       IRQs masked through the switch makes this transition indivisible. */
    bootstrap_finished = true;
    scheduler_yield();
    kernel_panic("Scheduler", "finished bootstrap resumed");
}

/* New threads enter here through the assembly trampoline. Returning is a fatal
   programming error: stacks and thread lifetime are permanent in this stage. */
void scheduler_run_current(void)
{
    threads[current].entry();
    kernel_panic("Scheduler", "thread returned");
}
