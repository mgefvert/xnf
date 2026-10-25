/*
 * Validate physical RAM before .bss and the final stack can be used.
 * Runs on the loaded temporary stack; no normal kernel services are available.
 */

#include "e820.h"

extern uint8_t kernel_start[] __asm__("__kernel_start");
extern uint8_t kernel_end[] __asm__("__kernel_end");

/* Before .bss, the normal console and allocator are unavailable. Use only
   the loaded image's temporary stack, read-only data, and raw VGA memory. */
static __attribute__((noreturn)) void boot_memory_panic(void)
{
    const char *message = "Boot memory panic: kernel extent is not usable RAM";
    volatile uint16_t *screen = (volatile uint16_t *)0xB8000u;
    while (*message) {
        *screen++ = 0x0700u | (uint8_t)*message++;
    }
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

/* This function MUST NOT read or write .bss, use the final kernel stack, or
   call normal kernel services. Keep in sync with memblock's conservative
   precedence: every enabled non-type-1 overlap blocks kernel placement. */
void boot_memory_check(void)
{
    uint64_t start = (uint32_t)kernel_start;
    uint64_t end = (uint32_t)kernel_end;
    uint64_t covered = start;
    uint32_t i;
    if (e820_detection_failed || e820_entry_count == 0 ||
        e820_entry_count > E820_MAX_ENTRIES || end <= start) {
        boot_memory_panic();
    }
    for (i = 0; i < e820_entry_count; ++i) {
        const E820Entry *entry = &e820_map[i];
        if ((entry->attributes & 1u) == 0 || entry->length == 0) {
            continue;
        }
        /* Check addition by subtraction before forming an exclusive end.
           Half-open intervals overlap iff each starts before the other ends. */
        if (entry->length > ~(uint64_t)0 - entry->base_address ||
            (entry->type != 1 && entry->base_address < end &&
             entry->base_address + entry->length > start)) {
            boot_memory_panic();
        }
    }
    /* BIOS records need not be sorted or merged. Repeatedly extend the covered
       prefix with any usable interval touching it; adjacent records are fine,
       but failure to advance proves there is an uncovered hole. Reserved
       overlaps were rejected above so usable records cannot conceal them. */
    while (covered < end) {
        uint64_t next = covered;
        for (i = 0; i < e820_entry_count; ++i) {
            const E820Entry *entry = &e820_map[i];
            if ((entry->attributes & 1u) != 0 && entry->type == 1 &&
                entry->base_address <= covered &&
                entry->base_address + entry->length > next) {
                next = entry->base_address + entry->length;
            }
        }
        if (next == covered) {
            boot_memory_panic();
        }
        covered = next;
    }
}
