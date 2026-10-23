#include "paging.h"
#include "memory.h"
#include "conio.h"

#define PAGE_SIZE 4096u
#define TABLE_ENTRIES 1024u
#define TABLE_SPAN (PAGE_SIZE * TABLE_ENTRIES)
#define PAGE_PRESENT 0x001u
#define PAGE_WRITABLE 0x002u
#define PAGE_FLAGS (PAGE_PRESENT | PAGE_WRITABLE)
#define PHYSICAL_LIMIT 0x100000000ULL
#define VGA_TEXT_END 0x000B9000u
#define CR0_PE 0x00000001u
#define CR0_PG 0x80000000u
#define CR4_PSE_PAE 0x00000030u

extern void paging_activate(uint32_t directory_physical);

/* GCC's asm name bypasses MinGW's C underscore decoration. */
extern uint8_t kernel_start[] __asm__("__kernel_start");
extern uint8_t kernel_end[] __asm__("__kernel_end");

uint32_t paging_directory_physical;

static __attribute__((noreturn)) void paging_panic(const char *reason)
{
    kprintf("Paging panic: %s\n", reason);
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

static uint32_t *allocate_page(void)
{
    uint64_t physical = memblock_alloc(PAGE_SIZE, PAGE_SIZE);
    uint32_t *page;
    uint32_t i;

    if (physical == 0 || physical > PHYSICAL_LIMIT - PAGE_SIZE) {
        paging_panic("no 32-bit RAM for paging structures");
    }
    page = (uint32_t *)(uint32_t)physical;
    for (i = 0; i < TABLE_ENTRIES; ++i) {
        page[i] = 0;
    }
    return page;
}

static uint64_t required_end(void)
{
    uint64_t end = TABLE_SPAN;
    uint32_t i;

    /* The linker bounds include the GDT, IDT, handlers, data, and stack.
       VGA text RAM is memory mapped; the PIC and CRTC use I/O ports. */
    if ((uint32_t)kernel_start != 0x10000u ||
        (uint32_t)kernel_end <= (uint32_t)kernel_start) {
        paging_panic("invalid kernel bounds");
    }
    if (end < (uint32_t)kernel_end) {
        end = (uint32_t)kernel_end;
    }
    if (end < VGA_TEXT_END) {
        end = VGA_TEXT_END;
    }

    /* Include existing allocations and each directory/table page as it is
       reserved. The allocator may place any of them above the first 4 MiB. */
    for (i = 0; i < memblock_region_count(); ++i) {
        const MemblockRegion *region = memblock_region(i);
        if (region->type == MEMBLOCK_BOOT_ALLOCATED &&
            end < region->base + region->length) {
            end = region->base + region->length;
        }
    }
    return end;
}

static void verify_tables(const uint32_t *directory, uint32_t table_count)
{
    uint32_t i;
    uint32_t j;

    for (i = 0; i < TABLE_ENTRIES; ++i) {
        uint32_t entry = directory[i];
        if (i >= table_count) {
            if (entry != 0) {
                paging_panic("unexpected directory entry");
            }
            continue;
        }
        if ((entry & (PAGE_SIZE - 1u)) != PAGE_FLAGS) {
            paging_panic("invalid directory flags");
        }
        {
            const uint32_t *table = (const uint32_t *)(entry & ~(PAGE_SIZE - 1u));
            for (j = 0; j < TABLE_ENTRIES; ++j) {
                uint32_t address = i * TABLE_SPAN + j * PAGE_SIZE;
                uint32_t expected = address == 0 ? 0 : address | PAGE_FLAGS;
                if (table[j] != expected) {
                    paging_panic("unexpected table entry");
                }
            }
        }
    }
}

void paging_prepare(void)
{
    uint32_t *directory;
    uint32_t table_count = 0;

    if (paging_directory_physical != 0) {
        paging_panic("paging structures already prepared");
    }
    directory = allocate_page();
    paging_directory_physical = (uint32_t)directory;

    /* Add one 4 MiB identity window at a time. Allocating a table can itself
       extend the required window, so recalculate after each allocation. */
    while ((uint64_t)table_count * TABLE_SPAN < required_end()) {
        uint32_t *table;
        uint32_t i;

        if (table_count == TABLE_ENTRIES) {
            paging_panic("physical address space exceeded");
        }
        table = allocate_page();
        for (i = 0; i < TABLE_ENTRIES; ++i) {
            uint32_t address = table_count * TABLE_SPAN + i * PAGE_SIZE;

            // Do not allocate page 0, we reserve this for null pointer dereference
            if (address != 0) {
                table[i] = address | PAGE_FLAGS;
            }
        }
        directory[table_count] = (uint32_t)table | PAGE_FLAGS;
        ++table_count;
    }

    verify_tables(directory, table_count);
    kprintf("Paging directory: %x  page tables: %d\n",
            paging_directory_physical, table_count);
    kprintf("Identity mapped physical range: [0x00001000, %X)\n",
            (uint64_t)table_count * TABLE_SPAN);
}

void paging_enable(void)
{
    uint32_t directory = paging_directory_physical;
    uint32_t cr0;
    uint32_t cr3;
    uint32_t cr4;

    if (directory == 0 || (directory & (PAGE_SIZE - 1u)) != 0) {
        paging_panic("directory missing or not page aligned");
    }
    __asm__ volatile ("mov %%cr0, %0" : "=r" (cr0) : : "memory");
    if ((cr0 & (CR0_PE | CR0_PG)) != CR0_PE) {
        paging_panic("expected protected mode with paging disabled");
    }

    /* Preparation covers code, data, stack, descriptors, handlers, VGA, and
       paging structures. Activation allocates nothing and changes no entries. */
    paging_activate(directory);

    __asm__ volatile ("mov %%cr0, %0" : "=r" (cr0) : : "memory");
    __asm__ volatile ("mov %%cr3, %0" : "=r" (cr3) : : "memory");
    __asm__ volatile ("mov %%cr4, %0" : "=r" (cr4) : : "memory");
    if ((cr0 & CR0_PG) == 0 || cr3 != directory ||
        (cr4 & CR4_PSE_PAE) != 0) {
        paging_panic("control register verification failed");
    }
    kprintf("Paging enabled: CR0=%x CR3=%x\n", cr0, cr3);
}
