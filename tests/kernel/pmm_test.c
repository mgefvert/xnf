/*
 * Guest-side physical ownership checks for the optional test build.
 * Never dereferences frame addresses, so synthetic E820 RAM need not exist in QEMU.
 */

#include "pmm.h"
#include "paging.h"
#include "kernel_test.h"
#include "panic.h"
#include "conio.h"

extern uint8_t kernel_start[] __asm__("__kernel_start");

static void require(bool condition, const char *reason)
{
    if (!condition) {
        kernel_panic("PMM test", reason);
    }
}

/* PMM owns frames, not their mappings. This test never dereferences physical
   addresses, including synthetic E820 frames not backed by guest RAM. */
void pmm_self_test(void)
{
    uint32_t before = pmm_free_pages();
    uint32_t pages[3];
    uint32_t i;
    /* Hold several loans simultaneously so duplicate frame ownership cannot
       hide behind a seemingly correct allocate/free counter sequence. */
    for (i = 0; i < 3; ++i) {
        uint32_t j;
        pages[i] = pmm_alloc_page();
        require(pages[i] != 0 && (pages[i] & (PAGE_SIZE - 1u)) == 0,
                "invalid physical allocation");
        for (j = 0; j < i; ++j) {
            require(pages[i] != pages[j], "duplicate allocation");
        }
    }
    require(pmm_free_pages() == before - 3u, "allocation counter");
    require(!pmm_free_page(pages[0] + 1u), "unaligned free accepted");
    for (i = 0; i < 3; ++i) {
        require(pmm_free_page(pages[i]), "page release");
    }
    require(!pmm_free_page(pages[0]), "double free accepted");
    require(!pmm_free_page((uint32_t)kernel_start), "kernel frame free accepted");
    require(!pmm_free_page(paging_directory_physical), "page directory free accepted");
    require(!pmm_free_page(0), "page zero free accepted");
    require(pmm_free_pages() == before, "release counter");
    kprintf("PMM allocation test: %x %x %x release OK\n", pages[0], pages[1], pages[2]);
    kprintf("PMM tests passed: reserved/zero/double-free rejected; free pages=%d\n",
            pmm_free_pages());
}
