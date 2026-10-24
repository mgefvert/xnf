#include "pmm.h"
#include "paging.h"
#include "conio.h"

extern uint8_t kernel_start[] __asm__("__kernel_start");

static void require(bool condition, const char *reason)
{
    if (!condition) {
        kprintf("PMM test failed: %s\n", reason);
        for (;;) {
            __asm__ volatile ("cli; hlt");
        }
    }
}

/* Inspect only the existing, identity-mapped paging structures. No mappings
   are installed here. Check both levels, including RW even with CR0.WP clear. */
static bool writable_identity_page(uint32_t physical)
{
    const uint32_t *directory = (const uint32_t *)paging_directory_physical;
    uint32_t cr0;
    uint32_t cr3;
    uint32_t entry;
    const uint32_t *table;

    __asm__ volatile ("mov %%cr0, %0" : "=r" (cr0) : : "memory");
    __asm__ volatile ("mov %%cr3, %0" : "=r" (cr3) : : "memory");
    if ((cr0 & 0x80000000u) == 0 || cr3 != paging_directory_physical) {
        return false;
    }
    entry = directory[physical >> 22];
    if ((entry & 0x83u) != 3u) {
        return false;
    }
    /* paging_prepare includes every paging structure in its identity map. */
    table = (const uint32_t *)(entry & 0xFFFFF000u);
    entry = table[(physical >> 12) & 1023u];
    return (entry & 3u) == 3u && (entry & 0xFFFFF000u) == physical;
}

void pmm_self_test(void)
{
    uint32_t before = pmm_free_pages();
    uint32_t pages[3];
    uint32_t i;
    uint32_t ordinary[3];

    for (i = 0; i < 3; ++i) {
        uint32_t j;
        pages[i] = pmm_alloc_page_low();
        require(pages[i] != 0 && pages[i] < PMM_LOW_LIMIT &&
                (pages[i] & (PMM_PAGE_SIZE - 1u)) == 0, "invalid low allocation");
        for (j = 0; j < i; ++j) {
            require(pages[i] != pages[j], "duplicate allocation");
        }
        require(writable_identity_page(pages[i]), "low page is not mapped writable");
    }
    require(pmm_free_pages() == before - 3u, "allocation counter");
    /* Write all three before checking any, so aliasing is detectable. */
    for (i = 0; i < 3; ++i) {
        volatile uint32_t *page = (volatile uint32_t *)pages[i];
        uint32_t j;
        for (j = 0; j < PMM_PAGE_SIZE / sizeof(uint32_t); ++j) {
            page[j] = 0xA55A0000u ^ pages[i] ^ j;
        }
    }
    for (i = 0; i < 3; ++i) {
        volatile const uint32_t *page = (volatile const uint32_t *)pages[i];
        uint32_t j;
        for (j = 0; j < PMM_PAGE_SIZE / sizeof(uint32_t); ++j) {
            require(page[j] == (0xA55A0000u ^ pages[i] ^ j), "page pattern");
        }
    }
    require(!pmm_free_page(pages[0] + 1u), "unaligned free accepted");
    for (i = 0; i < 3; ++i) {
        require(pmm_free_page(pages[i]), "low page release");
    }
    require(!pmm_free_page(pages[0]), "double free accepted");
    require(!pmm_free_page((uint32_t)kernel_start), "kernel frame free accepted");
    require(!pmm_free_page(paging_directory_physical), "page directory free accepted");
    require(!pmm_free_page(0), "page zero free accepted");
    require(pmm_free_pages() == before, "low release counter");
    kprintf("PMM low test: %x %x %x patterns/release OK\n", pages[0], pages[1], pages[2]);

    for (i = 0; i < 3; ++i) {
        uint32_t j;
        ordinary[i] = pmm_alloc_page();
        require(ordinary[i] != 0 && (ordinary[i] & (PMM_PAGE_SIZE - 1u)) == 0,
                "invalid general allocation");
        for (j = 0; j < i; ++j) {
            require(ordinary[i] != ordinary[j], "duplicate general allocation");
        }
    }
    require(pmm_free_pages() == before - 3u, "general allocation counter");
    /* Ordinary physical allocations may be unmapped: never dereference. */
    for (i = 0; i < 3; ++i) {
        require(pmm_free_page(ordinary[i]), "general page release");
    }
    require(pmm_free_pages() == before, "general release counter");
    kprintf("PMM general test: %x %x %x release OK\n",
            ordinary[0], ordinary[1], ordinary[2]);
    kprintf("PMM tests passed: reserved/zero/double-free rejected; free pages=%d\n",
            pmm_free_pages());
}
