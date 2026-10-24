#include "types.h"
#include "conio.h"
#include "memory.h"
#include "paging.h"
#include "pmm.h"

#define TIMER_DISPLAY_INTERVAL 18u

volatile uint32_t timer_ticks;

extern void interrupts_enable_and_halt(void);

void timer_handler(void)
{
    ++timer_ticks;
}

void kernel_main(void)
{
    uint32_t last_displayed_ticks = 0;

    clear_screen();
    display_e820_map();
    memblock_init();
    memblock_dump();

    paging_prepare();
    paging_enable();
    pmm_init();
    pmm_self_test();

    kprintf("Kernel started.\n");

    for (;;) {
        uint32_t ticks = timer_ticks;

        if ((uint32_t)(ticks - last_displayed_ticks) >= TIMER_DISPLAY_INTERVAL) {
            last_displayed_ticks = ticks;
            kputc('.');
        }

        /* STI; HLT in assembly enables IRQ0 and sleeps until the next IRQ. */
        interrupts_enable_and_halt();
    }
}
