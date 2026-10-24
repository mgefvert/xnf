#include "pmm.h"
#include "memory.h"
#include "conio.h"

#define PHYSICAL_END 0x100000000ULL
#define FRAME_COUNT (1024u * 1024u)
#define WORD_BITS 32u
#define BITMAP_WORDS (FRAME_COUNT / WORD_BITS)
#define LOW_WORDS (PMM_LOW_LIMIT / PMM_PAGE_SIZE / WORD_BITS)

/* One bit per physical frame: 1 = reserved or allocated, 0 = free.
   .bss is inside the linker-reserved kernel extent, including this 128 KiB. */
static uint32_t bitmap[BITMAP_WORDS];
_Static_assert(sizeof(bitmap) == 128u * 1024u, "PMM bitmap must cover 4 GiB");
static uint32_t managed_pages;
static uint32_t free_pages;
static uint32_t low_hint;
static uint32_t high_hint;
static bool initialized;

/* Single-CPU ownership and counter updates are atomic with respect to IRQs. */
static uint32_t irq_save(void)
{
    uint32_t flags;
    __asm__ volatile ("pushfl; popl %0; cli" : "=r" (flags) : : "memory");
    return flags;
}

static void irq_restore(uint32_t flags)
{
    __asm__ volatile ("pushl %0; popfl" : : "r" (flags) : "memory", "cc");
}

static __attribute__((noreturn)) void pmm_panic(const char *reason)
{
    kprintf("PMM panic: %s\n", reason);
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

/* The normalized map gives reserved E820 entries precedence independent of
   input order, and overlays kernel/boot reservations. Use it rather than
   reinterpreting raw E820 entries or maintaining a second reservation bitmap. */
static bool frame_is_managed(uint32_t address)
{
    uint32_t i;
    uint64_t end = (uint64_t)address + PMM_PAGE_SIZE;

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
        pmm_panic("already initialized");
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
        first = (uint32_t)((region->base + PMM_PAGE_SIZE - 1u) / PMM_PAGE_SIZE);
        last = (uint32_t)(end / PMM_PAGE_SIZE);
        if (first == 0) {
            first = 1;
        }
        for (frame = first; frame < last; ++frame) {
            bitmap[frame / WORD_BITS] &= ~(1u << (frame % WORD_BITS));
            ++managed_pages;
        }
    }
    free_pages = managed_pages;
    low_hint = 0;
    high_hint = LOW_WORDS;
    /* Reservations have been imported. With IRQs still disabled, freeze the
       bootstrap pool before publishing PMM ownership or restoring IF. */
    memblock_retire();
    initialized = true;
    kprintf("PMM: managed usable pages=%d free pages=%d\n", managed_pages, free_pages);
    kprintf("Physical page ownership transferred to PMM; bootstrap allocator retired.\n");
    irq_restore(flags);
}

/* Word-aligned limits keep low/high pools disjoint. Hints avoid rescanning
   full words on each allocation, and wrap so freed frames can be reused. */
static uint32_t allocate_words(uint32_t first, uint32_t last, uint32_t *hint)
{
    uint32_t word = *hint;
    uint32_t remaining = last - first;

    while (remaining-- != 0) {
        if (bitmap[word] != 0xFFFFFFFFu) {
            uint32_t bit = (uint32_t)__builtin_ctz(~bitmap[word]);
            bitmap[word] |= 1u << bit;
            --free_pages;
            *hint = word;
            return (word * WORD_BITS + bit) * PMM_PAGE_SIZE;
        }
        if (++word == last) {
            word = first;
        }
    }
    return 0;
}

uint32_t pmm_alloc_page(void)
{
    uint32_t flags = irq_save();
    uint32_t address = 0;

    if (initialized && free_pages != 0) {
        address = allocate_words(LOW_WORDS, BITMAP_WORDS, &high_hint);
        if (address == 0) {
            address = allocate_words(0, LOW_WORDS, &low_hint);
        }
    }
    irq_restore(flags);
    return address;
}

uint32_t pmm_alloc_page_low(void)
{
    uint32_t flags = irq_save();
    uint32_t address = 0;

    if (initialized && free_pages != 0) {
        address = allocate_words(0, LOW_WORDS, &low_hint);
    }
    irq_restore(flags);
    return address;
}

bool pmm_free_page(uint32_t physical_address)
{
    uint32_t flags = irq_save();
    uint32_t frame = physical_address / PMM_PAGE_SIZE;
    uint32_t mask = 1u << (frame % WORD_BITS);
    bool success = false;

    if (initialized && physical_address != 0 &&
        (physical_address & (PMM_PAGE_SIZE - 1u)) == 0 &&
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

uint32_t pmm_free_pages(void)
{
    /* Aligned 32-bit reads are atomic on this single-CPU kernel. */
    return free_pages;
}
