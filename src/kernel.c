/*
 * Kernel initialization and the two sleeping demonstration threads.
 * Orders memory-manager handovers and keeps console work outside IRQ handlers.
 */

#include "types.h"
#include "conio.h"
#include "memory.h"
#include "paging.h"
#include "pmm.h"
#include "vmm.h"
#include "kheap.h"
#include "object.h"
#include "panic.h"
#include "scheduler.h"
#ifdef XNF_BUILD_TESTS
#include "kernel_test.h"
#endif

/* Channel 0 is programmed for approximately 100 ticks per second. */
#define THREAD_DISPLAY_INTERVAL 100u

/* IRQ0 changes this asynchronously. Volatile forces normal code to reload
   it; an aligned 32-bit read is atomic on the current single CPU. */
volatile uint32_t timer_ticks;

extern void interrupts_enable_and_halt(void);

void timer_handler(void)
{
    /* Do no printing or allocation here: the IRQ may interrupt those services
       while their normal-context state is only partly updated. */
    ++timer_ticks;
}

static void print_a(void)
{
    for (;;) {
        kputc('A');
        if (!scheduler_sleep(THREAD_DISPLAY_INTERVAL)) kernel_panic("Thread A", "sleep failed");
    }
}

static void print_b(void)
{
    for (;;) {
        kputc('B');
        if (!scheduler_sleep(THREAD_DISPLAY_INTERVAL)) kernel_panic("Thread B", "sleep failed");
    }
}

/* Test guests may redirect here after initialization without changing their
   default allocator fixtures. New threads inherit the enabled IF. */
void kernel_demo_start(void)
{
    __asm__ volatile ("sti" : : : "memory");
    if (!scheduler_start(print_a, print_b)) kernel_panic("Scheduler", "thread startup failed");
    scheduler_finish_bootstrap();
}

void kernel_main(void)
{
#ifdef XNF_BUILD_TESTS
    uint32_t last_displayed_ticks = 0;
#endif

    clear_screen();
    display_e820_map();
    memblock_init();
    memblock_dump();

    /* Build translations while direct physical access is still possible,
       activate them, then transfer physical ownership from memblock to PMM.
       VMM depends on both working translations and the retired bootstrap pool. */
    paging_prepare();
    paging_enable();
    pmm_init();
#ifdef XNF_BUILD_TESTS
    pmm_self_test();
#endif
    vmm_init();
    kheap_init();
    if (object_manager_init() != OBJECT_OK) kernel_panic("Object", "initialization failed");

    kprintf("Kernel started.\n");

#ifdef XNF_BUILD_TESTS
    /* Keep host-selected test entries and synthetic low-memory fixtures
       independent of the permanent demonstration stacks. */
    for (;;) {
        uint32_t ticks = timer_ticks;

        /* Unsigned subtraction handles a wrapping tick counter. The PIT's
           integer divisor makes the one-second interval approximate. */
        if ((uint32_t)(ticks - last_displayed_ticks) >= THREAD_DISPLAY_INTERVAL) {
            last_displayed_ticks = ticks;
            kputc('.');
        }

        /* STI; HLT in assembly enables IRQ0 and sleeps until the next IRQ. */
        interrupts_enable_and_halt();
    }
#else
    kernel_demo_start();
#endif
}
