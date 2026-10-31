/*
 * Disposable guest exercises for voluntary and timer-driven context switches.
 * Workers touch only private stacks and test counters, never shared services.
 */
#include "types.h"
#include "scheduler.h"
#include "irq.h"
#include "vmm.h"
#include "pmm.h"

volatile uint32_t scheduler_test_stage;
volatile uint32_t scheduler_test_error;
volatile uint32_t scheduler_test_counts[2];
volatile uint32_t scheduler_test_stacks[2];
static uint32_t cooperative;
extern volatile uint32_t timer_ticks;

/* Keep distinctive general-register values live across a software yield and
   through arbitrary timer interrupts. Also check that IRETD preserves IF/DF. */
static uint32_t check_registers(uint32_t yield, uint32_t salt)
{
    uint32_t bad = 0, before, after;
    __asm__ volatile (
        "movl $0x12345678, %%eax\n\t"
        "movl $0x23456789, %%ebx\n\t"
        "movl $0x3456789a, %%ecx\n\t"
        "movl $0x456789ab, %%edx\n\t"
        "movl $0x56789abc, %%esi\n\t"
        "movl $0x6789abcd, %%edi\n\t"
        /* Each thread uses distinct canaries, so restoring another thread's
           registers cannot accidentally pass this check. */
        "addl %[salt], %%eax\n\t"
        "addl %[salt], %%ebx\n\t"
        "addl %[salt], %%ecx\n\t"
        "addl %[salt], %%edx\n\t"
        "addl %[salt], %%esi\n\t"
        "addl %[salt], %%edi\n\t"
        /* IRETD must restore DF even though interrupt entry clears it for C.
           Clear it again before leaving this block to obey the C ABI. */
        "std\n\t"
        "pushfl\n\t"
        "popl %[before]\n\t"
        "cmpl $0, %[yield]\n\t"
        "je 1f\n\t"
        "int $0x30\n\t"
        "1: pushfl\n\t"
        "popl %[after]\n\t"
        "cld\n\t"
        "subl %[salt], %%eax\n\t"
        "subl %[salt], %%ebx\n\t"
        "subl %[salt], %%ecx\n\t"
        "subl %[salt], %%edx\n\t"
        "subl %[salt], %%esi\n\t"
        "subl %[salt], %%edi\n\t"
        "cmpl $0x12345678, %%eax\n\t"
        "jne 2f\n\t"
        "cmpl $0x23456789, %%ebx\n\t"
        "jne 2f\n\t"
        "cmpl $0x3456789a, %%ecx\n\t"
        "jne 2f\n\t"
        "cmpl $0x456789ab, %%edx\n\t"
        "jne 2f\n\t"
        "cmpl $0x56789abc, %%esi\n\t"
        "jne 2f\n\t"
        "cmpl $0x6789abcd, %%edi\n\t"
        "je 3f\n\t"
        "2: movl $1, %[bad]\n\t"
        "3:"
        : [bad] "+m" (bad), [before] "=m" (before), [after] "=m" (after)
        : [yield] "m" (yield), [salt] "m" (salt)
        : "eax", "ebx", "ecx", "edx", "esi", "edi", "cc", "memory");
    return bad | ((before ^ after) & 0x600u) | ((before & 0x400u) ? 0u : 1u);
}

static void worker(uint32_t index)
{
    volatile uint32_t cookie = 0xabc00000u + index;
    scheduler_test_stacks[index] = (uint32_t)&cookie;
    for (;;) {
        if (cookie != 0xabc00000u + index || check_registers(cooperative, index + 1u))
            scheduler_test_error = index + 1u;
        ++scheduler_test_counts[index];
    }
}

static void first(void) { worker(0); }
static void second(void) { worker(1); }

/* GDB enters these with a fresh bootstrap stack after ordinary boot. */
void scheduler_test_cooperative(void)
{
    uint32_t free_before;
    (void)irq_save();
    cooperative = 1;
    free_before = vmm_free_page_count();
    if (scheduler_start(0, second) || scheduler_start(first, 0) ||
        vmm_free_page_count() != free_before) scheduler_test_error = 11;
    if (!scheduler_start(first, second)) scheduler_test_error = 10;
    {
        uint32_t flags;
        __asm__ volatile ("pushfl; popl %0" : "=r" (flags));
        if (flags & 0x200u) scheduler_test_error = 13;
    }
    if (scheduler_start(first, second) || vmm_free_page_count() != free_before - 8u)
        scheduler_test_error = 12;
    while (!scheduler_test_error &&
           (scheduler_test_counts[0] < 1000 || scheduler_test_counts[1] < 1000))
        scheduler_yield();
    scheduler_test_stage = 1;
    for (;;) __asm__ volatile ("cli; hlt");
}

void scheduler_test_preemptive(void)
{
    uint32_t initial_ticks;
    (void)irq_save();
    cooperative = 0;
    /* New frames inherit enabled IF. Scheduler setup masks IRQs internally. */
    __asm__ volatile ("sti" : : : "memory");
    if (!scheduler_start(first, second)) scheduler_test_error = 10;
    {
        uint32_t flags;
        __asm__ volatile ("pushfl; popl %0" : "=r" (flags));
        if (!(flags & 0x200u)) scheduler_test_error = 13;
    }
    initial_ticks = timer_ticks;
    while (!scheduler_test_error &&
           (!scheduler_test_counts[0] || !scheduler_test_counts[1] ||
            (uint32_t)(timer_ticks - initial_ticks) < 12u)) { }
    scheduler_test_stage = 2;
    for (;;) { }
}

/* Disposable guest: leave seven frames so the first four-page stack succeeds
   and the second fails partway through. Keep one loan to make retry possible. */
void scheduler_test_failure(void)
{
    uint32_t retained = 0, virtual_before;
    (void)irq_save();
    while (pmm_free_pages() > 7u) {
        uint32_t page = pmm_alloc_page();
        if (!page) { scheduler_test_error = 20; break; }
        if (!retained) retained = page;
    }
    virtual_before = vmm_free_page_count();
    if (!retained || pmm_free_pages() != 7u || scheduler_start(first, second) ||
        pmm_free_pages() != 7u || vmm_free_page_count() != virtual_before)
        scheduler_test_error = 21;
    if (!pmm_free_page(retained) || !scheduler_start(first, second) ||
        pmm_free_pages() != 0 || vmm_free_page_count() != virtual_before - 8u)
        scheduler_test_error = 22;
    scheduler_test_stage = 3;
    for (;;) __asm__ volatile ("cli; hlt");
}
