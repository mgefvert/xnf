#include "panic.h"
#include "conio.h"

void kernel_panic(const char *subsystem, const char *reason)
{
    __asm__ volatile ("cli" : : : "memory");
    kprintf("%s panic: %s\n", subsystem, reason);
    for (;;) {
        __asm__ volatile ("hlt");
    }
}
