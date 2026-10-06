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

    clear_screen();
    display_e820_map();

    for (;;) {
        uint32_t ticks = timer_ticks;

        if ((uint32_t)(ticks - last_displayed_ticks) >= TIMER_DISPLAY_INTERVAL) {
            last_displayed_ticks = ticks;
            kputs("Timer ticks: ");
            kprint_dec(ticks);
            kputc('\n');
        }

        /* STI; HLT in assembly enables IRQ0 and sleeps until the next IRQ. */
        interrupts_enable_and_halt();
    }
}
