/*
 * Minimal round-robin scheduler for a bootstrap context and two kernel threads.
 * Interrupt assembly owns frame entry/exit; all contexts share one address space.
 */
#include "scheduler.h"
#include "irq.h"
#include "vmm.h"
#include "panic.h"

#define THREAD_COUNT 3u
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
} Thread;

static Thread threads[THREAD_COUNT];
static uint32_t current;
static bool active;

extern void scheduler_thread_entry(void);

static void prepare_thread(Thread *thread, void *stack, KernelThreadEntry entry,
                           uint32_t flags)
{
    /* VMM returns zeroed pages. Place the artificial frame at the high end so
       the first IRETD exposes an aligned, empty downward-growing stack. */
    ThreadFrame *frame = (ThreadFrame *)((uint8_t *)stack + STACK_BYTES - sizeof(ThreadFrame));
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
    prepare_thread(&threads[1], first_stack, first, flags);
    prepare_thread(&threads[2], second_stack, second, flags);
    current = 0;
    active = true;
    irq_restore(flags);
    return true;
}

/* Called only from interrupt gates with IF clear. Before startup, return the
   same frame so ordinary boot and its disposable tests retain their behavior. */
ThreadFrame *scheduler_select(ThreadFrame *frame)
{
    if (!active) return frame;
    threads[current].frame = frame;
    current = (current + 1u) % THREAD_COUNT;
    return threads[current].frame;
}

/* New threads enter here through the assembly trampoline. Returning is a fatal
   programming error: stacks and thread lifetime are permanent in this stage. */
void scheduler_run_current(void)
{
    threads[current].entry();
    kernel_panic("Scheduler", "thread returned");
}
