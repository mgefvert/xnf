/*
 * Kernel initialization and the timer-driven idle loop.
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
#ifdef XNF_BUILD_TESTS
#include "kernel_test.h"
#endif

#define TIMER_DISPLAY_INTERVAL 18u

/* IRQ0 changes this asynchronously. Volatile forces the idle loop to reload
   it; an aligned 32-bit read is atomic on the current single CPU. */
volatile uint32_t timer_ticks;

extern void interrupts_enable_and_halt(void);

void timer_handler(void)
{
    /* Do no printing or allocation here: the IRQ may interrupt those services
       while their normal-context state is only partly updated. */
    ++timer_ticks;
}

void kernel_main(void)
{
    uint32_t last_displayed_ticks = 0;

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

    for (;;) {
        uint32_t ticks = timer_ticks;

        /* Unsigned subtraction handles a wrapping tick counter. This is
           approximate elapsed time: XNF has not programmed the firmware PIT. */
        if ((uint32_t)(ticks - last_displayed_ticks) >= TIMER_DISPLAY_INTERVAL) {
            last_displayed_ticks = ticks;
            kputc('.');
        }

        /* STI; HLT in assembly enables IRQ0 and sleeps until the next IRQ. */
        interrupts_enable_and_halt();
    }
}
