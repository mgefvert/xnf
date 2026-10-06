#include "types.h"
#include "conio.h"
#include "memory.h"

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
    uint64_t small_allocation;
    uint64_t page_allocation;

    clear_screen();
    display_e820_map();
    memblock_init();
    memblock_dump();

    small_allocation = memblock_alloc(100, 16);
    page_allocation = memblock_alloc(8192, 4096);
    kprintf("Early allocations: 100/16=%X 8192/4096=%X\n",
            small_allocation, page_allocation);
    memblock_dump();

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
