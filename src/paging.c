/*
 * Build and inspect non-PAE x86 page tables, then edit kernel mappings.
 * Owns translation mechanics; VMM separately decides virtual and physical ownership.
 */

#include "paging.h"
#include "memory.h"
#include "conio.h"
#include "panic.h"
#include "paging_internal.h"
#include "irq.h"

#define TABLE_ENTRIES 1024u
/* A non-PAE address splits into directory[31:22], table[21:12], offset[11:0].
   Each table has 1024 four-byte entries and covers 1024 * 4 KiB = 4 MiB. */
#define TABLE_SPAN (PAGE_SIZE * TABLE_ENTRIES)
#define KERNEL_TABLES (KERNEL_VIRTUAL_END / TABLE_SPAN)
#define PAGE_PRESENT 0x001u
#define PAGE_FLAGS (PAGE_PRESENT | PAGING_WRITABLE)
#define VGA_TEXT_END 0x000B9000u
#define CR0_PE 0x00000001u
#define CR0_PG 0x80000000u
#define CR0_WP 0x00010000u
#define CR4_PSE_PAE 0x00000030u

extern void paging_activate(uint32_t directory_physical);

/* GCC's asm name bypasses MinGW's C underscore decoration. */
extern uint8_t kernel_start[] __asm__("__kernel_start");
extern uint8_t kernel_end[] __asm__("__kernel_end");

uint32_t paging_directory_physical;
uint32_t paging_identity_end;

/* All runtime paging structures remain in the permanent identity prefix.
   Preallocate 256 KiB rather than needing a mapping to create a new table. */
static uint32_t kernel_tables[KERNEL_TABLES][TABLE_ENTRIES]
    __attribute__((aligned(PAGE_SIZE)));
static uint32_t kernel_directory[TABLE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
static bool enabled;

static uint64_t required_end(void)
{
    uint64_t end = TABLE_SPAN;
    uint32_t i;

    /* The linker bounds include the GDT, IDT, handlers, data, and stack.
       VGA text RAM is memory mapped; the PIC and CRTC use I/O ports. */
    if ((uint32_t)kernel_start != 0x10000u ||
        (uint32_t)kernel_end <= (uint32_t)kernel_start) {
        kernel_panic("Paging", "invalid kernel bounds");
    }
    if (end < (uint32_t)kernel_end) {
        end = (uint32_t)kernel_end;
    }
    if (end < VGA_TEXT_END) {
        end = VGA_TEXT_END;
    }

    /* Include any earlier bootstrap allocations. All paging structures are
       static and already covered by the linker bounds; this allocates nothing. */
    for (i = 0; i < memblock_region_count(); ++i) {
        const MemblockRegion *region = memblock_region(i);
        if (region->type == MEMBLOCK_BOOT_ALLOCATED &&
            end < region->base + region->length) {
            end = region->base + region->length;
        }
    }
    return end;
}

static void verify_tables(const uint32_t *directory)
{
    uint32_t i;
    uint32_t j;

    /* This exact comparison is safe before activation only. Once the CPU
       walks these tables it may set accessed/dirty bits without asking C. */
    for (i = 0; i < TABLE_ENTRIES; ++i) {
        uint32_t entry = directory[i];
        if (i >= KERNEL_TABLES) {
            if (entry != 0) {
                kernel_panic("Paging", "unexpected directory entry");
            }
            continue;
        }
        if (entry != ((uint32_t)kernel_tables[i] | PAGE_FLAGS)) {
            kernel_panic("Paging", "invalid directory flags");
        }
        {
            /* A page-aligned physical base occupies the upper 20 bits;
               clearing the low 12 flag bits recovers its identity pointer. */
            const uint32_t *table = (const uint32_t *)(entry & ~(PAGE_SIZE - 1u));
            for (j = 0; j < TABLE_ENTRIES; ++j) {
                uint32_t address = i * TABLE_SPAN + j * PAGE_SIZE;
                uint32_t expected = address == 0 || address >= paging_identity_end
                    ? 0 : address | PAGE_FLAGS;
                if (table[j] != expected) {
                    kernel_panic("Paging", "unexpected table entry");
                }
            }
        }
    }
}

void paging_prepare(void)
{
    uint32_t *directory;
    uint32_t i;
    uint64_t end;

    if (paging_directory_physical != 0) {
        kernel_panic("Paging", "paging structures already prepared");
    }
    directory = kernel_directory;
    paging_directory_physical = (uint32_t)directory;

    /* All paging structures are reserved in .bss before preparation begins. */
    /* Round upward to a whole table span. The cast keeps the complement
       64-bit, preserving a possible exclusive physical end at 4 GiB. */
    end = (required_end() + TABLE_SPAN - 1u) & ~((uint64_t)TABLE_SPAN - 1u);
    if (end >= KERNEL_VIRTUAL_END) {
        kernel_panic("Paging", "bootstrap mappings leave no kernel virtual arena");
    }
    paging_identity_end = (uint32_t)end;
    for (i = 0; i < KERNEL_TABLES; ++i) {
        uint32_t j;
        for (j = 0; j < TABLE_ENTRIES; ++j) {
            uint32_t address = i * TABLE_SPAN + j * PAGE_SIZE;
            kernel_tables[i][j] = address != 0 && address < paging_identity_end
                ? address | PAGE_FLAGS : 0;
        }
        /* Tables exist even for currently unused virtual ranges. Their PTEs
           remain zero until VMM supplies backing; no USER bit is ever set. */
        directory[i] = (uint32_t)kernel_tables[i] | PAGE_FLAGS;
    }

    verify_tables(directory);
    kprintf("Paging directory: %x  page tables: %d\n",
            paging_directory_physical, KERNEL_TABLES);
    kprintf("Identity mapped physical range: [0x00001000, %X)\n",
            (uint64_t)paging_identity_end);
}

void paging_enable(void)
{
    uint32_t directory = paging_directory_physical;
    uint32_t cr0;
    uint32_t cr3;
    uint32_t cr4;

    if (directory == 0 || (directory & (PAGE_SIZE - 1u)) != 0) {
        kernel_panic("Paging", "directory missing or not page aligned");
    }
    __asm__ volatile ("mov %%cr0, %0" : "=r" (cr0) : : "memory");
    if ((cr0 & (CR0_PE | CR0_PG)) != CR0_PE) {
        kernel_panic("Paging", "expected protected mode with paging disabled");
    }

    /* Preparation covers code, data, stack, descriptors, handlers, VGA, and
       paging structures. Activation allocates nothing and changes no entries. */
    paging_activate(directory);

    __asm__ volatile ("mov %%cr0, %0" : "=r" (cr0) : : "memory");
    __asm__ volatile ("mov %%cr3, %0" : "=r" (cr3) : : "memory");
    __asm__ volatile ("mov %%cr4, %0" : "=r" (cr4) : : "memory");
    if ((cr0 & (CR0_PG | CR0_WP)) != (CR0_PG | CR0_WP) || cr3 != directory ||
        (cr4 & CR4_PSE_PAE) != 0) {
        kernel_panic("Paging", "control register verification failed");
    }
    enabled = true;
    kprintf("Paging enabled: CR0=%x CR3=%x\n", cr0, cr3);
}

bool paging_query(uint32_t virtual_address, PagingMapping *mapping)
{
    uint32_t flags = irq_save();
    bool found = false;
    if (enabled && mapping != 0 && virtual_address < KERNEL_VIRTUAL_END) {
        const uint32_t *directory = (const uint32_t *)paging_directory_physical;
        /* Discard the offset/table bits for the PDE index; shifting by 12
           and masking ten bits gives the PTE index within that table. */
        uint32_t pde = directory[virtual_address >> 22];
        uint32_t pte = kernel_tables[virtual_address >> 22][(virtual_address >> 12) & 1023u];
        /* 0x81 tests PRESENT (bit 0) and PS (bit 7): require a present 4 KiB
           table, not a 4 MiB large page. Both levels must permit a write. */
        if ((pde & 0x81u) == PAGE_PRESENT && (pte & PAGE_PRESENT) != 0) {
            mapping->physical_address = (pte & 0xFFFFF000u) | (virtual_address & 4095u);
            mapping->flags = (pde & pte) & PAGING_WRITABLE;
            found = true;
        }
    }
    irq_restore(flags);
    return found;
}

/* Each edit excludes IRQs only around the PTE update and local invalidation.
   VMM owns allocation transactions in non-preemptive normal context. */
static uint32_t *dynamic_entry(uint32_t address)
{
    /* Centralize the boundary checks so no runtime operation can unmap its
       own code/stack/tables or consume the future user virtual range. */
    if (!enabled || address < paging_identity_end || address >= KERNEL_VIRTUAL_END ||
        (address & (PAGE_SIZE - 1u)) != 0) {
        return 0;
    }
    return &kernel_tables[address >> 22][(address >> 12) & 1023u];
}

static void invalidate(uint32_t address)
{
    /* Editing RAM does not evict a cached CPU translation. INVLPG makes this
       virtual page use the new entry; the clobber orders compiler memory use.
       This affects the current CPU only; SMP would require remote shootdowns. */
    __asm__ volatile ("invlpg (%0)" : : "r" (address) : "memory");
}

bool paging_map(uint32_t virtual_address, uint32_t physical_address, uint32_t flags)
{
    uint32_t saved = irq_save();
    uint32_t *entry = dynamic_entry(virtual_address);
    bool result = false;
    /* Requiring an empty slot prevents replacement from silently orphaning
       someone else's frame. OR flags into the aligned base; they occupy the
       low bits which the address itself must leave clear. */
    if (entry != 0 && *entry == 0 && physical_address != 0 &&
        (physical_address & (PAGE_SIZE - 1u)) == 0 && (flags & ~PAGING_WRITABLE) == 0) {
        *entry = physical_address | PAGE_PRESENT | flags;
        invalidate(virtual_address);
        result = true;
    }
    irq_restore(saved);
    return result;
}

bool paging_unmap(uint32_t virtual_address)
{
    uint32_t saved = irq_save();
    uint32_t *entry = dynamic_entry(virtual_address);
    bool result = false;
    if (entry != 0 && (*entry & PAGE_PRESENT) != 0) {
        /* Revoke access before the owner can return this frame to PMM. */
        *entry = 0;
        invalidate(virtual_address);
        result = true;
    }
    irq_restore(saved);
    return result;
}

bool paging_protect(uint32_t virtual_address, uint32_t flags)
{
    uint32_t saved = irq_save();
    uint32_t *entry = dynamic_entry(virtual_address);
    bool result = false;
    if (entry != 0 && (*entry & PAGE_PRESENT) != 0 && (flags & ~PAGING_WRITABLE) == 0) {
        /* Preserve the physical address and hardware accessed/dirty bits. */
        *entry = (*entry & ~PAGING_WRITABLE) | flags;
        invalidate(virtual_address);
        result = true;
    }
    irq_restore(saved);
    return result;
}
