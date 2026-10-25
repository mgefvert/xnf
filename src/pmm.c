#include "pmm.h"
#include "memory.h"
#include "conio.h"
#include "panic.h"
#include "irq.h"

#define FRAME_COUNT (1024u * 1024u)
#define WORD_BITS 32u
#define BITMAP_WORDS (FRAME_COUNT / WORD_BITS)

/* One bit per physical frame: 1 = reserved or allocated, 0 = free.
   .bss is inside the linker-reserved kernel extent, including this 128 KiB. */
static uint32_t bitmap[BITMAP_WORDS];
_Static_assert(sizeof(bitmap) == 128u * 1024u, "PMM bitmap must cover 4 GiB");
static uint32_t managed_pages;
static uint32_t free_pages;
static uint32_t next_word;
static bool initialized;

/* The normalized map gives reserved E820 entries precedence independent of
   input order, and overlays kernel/boot reservations. Use it rather than
   reinterpreting raw E820 entries or maintaining a second reservation bitmap. */
static bool frame_is_managed(uint32_t address)
{
    uint32_t i;
    uint64_t end = (uint64_t)address + PAGE_SIZE;

    for (i = 0; i < memblock_region_count(); ++i) {
        const MemblockRegion *region = memblock_region(i);
        if (region->base > address) {
            return false;
        }
        if (region->type == MEMBLOCK_AVAILABLE && region->base <= address &&
            end <= region->base + region->length) {
            return true;
        }
    }
    return false;
}

void pmm_init(void)
{
    uint32_t flags = irq_save();
    uint32_t i;

    if (initialized) {
        kernel_panic("PMM", "already initialized");
    }
    for (i = 0; i < BITMAP_WORDS; ++i) {
        bitmap[i] = 0xFFFFFFFFu;
    }
    for (i = 0; i < memblock_region_count(); ++i) {
        const MemblockRegion *region = memblock_region(i);
        uint64_t end;
        uint32_t first;
        uint32_t last;
        uint32_t frame;

        if (region->type != MEMBLOCK_AVAILABLE || region->base >= PHYSICAL_END) {
            continue;
        }
        /* memblock_init already rejected E820 address/length overflow.
           Clip before rounding; a partial page is never made available. */
        end = region->base + region->length;
        if (end > PHYSICAL_END) {
            end = PHYSICAL_END;
        }
        first = (uint32_t)((region->base + PAGE_SIZE - 1u) / PAGE_SIZE);
        last = (uint32_t)(end / PAGE_SIZE);
        if (first == 0) {
            first = 1;
        }
        for (frame = first; frame < last; ++frame) {
            bitmap[frame / WORD_BITS] &= ~(1u << (frame % WORD_BITS));
            ++managed_pages;
        }
    }
    free_pages = managed_pages;
    next_word = 0;
    /* Reservations have been imported. With IRQs still disabled, freeze the
       bootstrap pool before publishing PMM ownership or restoring IF. */
    memblock_retire();
    initialized = true;
    kprintf("PMM: managed usable pages=%d free pages=%d\n", managed_pages, free_pages);
    kprintf("Physical page ownership transferred to PMM; bootstrap allocator retired.\n");
    irq_restore(flags);
}

/* One circular search over all physical frames. No virtual-address policy
   belongs here; the hint skips full words and wraps to reuse freed frames. */
static uint32_t allocate_frame(void)
{
    uint32_t word = next_word;
    uint32_t remaining = BITMAP_WORDS;

    while (remaining-- != 0) {
        if (bitmap[word] != 0xFFFFFFFFu) {
            uint32_t bit = (uint32_t)__builtin_ctz(~bitmap[word]);
            bitmap[word] |= 1u << bit;
            --free_pages;
            next_word = word;
            return (word * WORD_BITS + bit) * PAGE_SIZE;
        }
        if (++word == BITMAP_WORDS) {
            word = 0;
        }
    }
    return 0;
}

uint32_t pmm_alloc_page(void)
{
    uint32_t flags = irq_save();
    uint32_t address = 0;

    if (initialized && free_pages != 0) {
        address = allocate_frame();
    }
    irq_restore(flags);
    return address;
}

bool pmm_free_page(uint32_t physical_address)
{
    uint32_t flags = irq_save();
    uint32_t frame = physical_address / PAGE_SIZE;
    uint32_t mask = 1u << (frame % WORD_BITS);
    bool success = false;

    if (initialized && physical_address != 0 &&
        (physical_address & (PAGE_SIZE - 1u)) == 0 &&
        frame_is_managed(physical_address) &&
        (bitmap[frame / WORD_BITS] & mask) != 0) {
        bitmap[frame / WORD_BITS] &= ~mask;
        ++free_pages;
        success = true;
    }
    irq_restore(flags);
    return success;
}

uint32_t pmm_total_pages(void)
{
    return managed_pages;
}

bool pmm_is_initialized(void)
{
    return initialized;
}

uint32_t pmm_free_pages(void)
{
    /* Aligned 32-bit reads are atomic on this single-CPU kernel. */
    return free_pages;
}
