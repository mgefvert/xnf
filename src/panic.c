/*
 * Shared fatal-error path after the normal console is initialized.
 * Disables maskable interrupts before reporting the error and halting permanently.
 */

#include "panic.h"
#include "conio.h"

void kernel_panic(const char *subsystem, const char *reason)
{
    /* Keep the fatal report and terminal state together; a panic must not
       return to a caller whose memory-management invariants may be broken. */
    __asm__ volatile ("cli" : : : "memory");
    kprintf("%s panic: %s\n", subsystem, reason);
    for (;;) {
        __asm__ volatile ("hlt");
    }
}
