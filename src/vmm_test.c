#include "vmm.h"
#include "paging_internal.h"
#include "pmm.h"
#include "irq.h"
#include "conio.h"

/* Inspected by the disposable QEMU harness, not used by allocator policy. */
volatile uint32_t vmm_test_stage;
volatile uint32_t vmm_test_high_physical;
volatile uint32_t vmm_test_fault_address;

static void require(bool condition, const char *reason)
{
    if (!condition) {
        kprintf("VMM test failed: %s\n", reason);
        for (;;) {
            __asm__ volatile ("cli; hlt");
        }
    }
}

static uint32_t read_flags(void)
{
    uint32_t flags;
    __asm__ volatile ("pushfl; popl %0" : "=r" (flags));
    return flags;
}

void vmm_self_test(void)
{
    uint32_t original_flags = read_flags();
    uint32_t physical_before = pmm_free_pages();
    uint32_t virtual_before = vmm_free_page_count();
    uint32_t i;
    uint32_t flags;
    uint32_t count;
    uint32_t *a;
    uint32_t *b;
    uint32_t *c;
    uint32_t loans[3];
    PagingMapping first;
    PagingMapping second;
    PagingMapping queried;

    clear_screen();
    vmm_test_stage = 1;
    require(vmm_alloc_pages(0) == 0 && vmm_alloc_pages(0xFFFFFFFFu) == 0,
            "invalid allocation sizes");
    require(!vmm_free_pages(0) && !vmm_free_pages((void *)0x10000u) &&
            !vmm_free_pages((void *)KERNEL_VIRTUAL_END), "invalid frees");
    require(!paging_query(0, &queried) &&
            !paging_query(KERNEL_VIRTUAL_END, &queried) &&
            !paging_query(paging_identity_end, 0), "query validation");

    /* 1025 pages cross both page and 4 MiB directory boundaries. */
    a = vmm_alloc_pages(1025);
    b = vmm_alloc_pages(2);
    require(a != 0 && b != 0, "initial allocations");
    require((uint32_t)a == paging_identity_end &&
            (uint32_t)b == (uint32_t)a + 1025u * PAGING_PAGE_SIZE, "virtual runs");
    require(pmm_free_pages() == physical_before - 1027u &&
            vmm_free_page_count() == virtual_before - 1027u, "allocation counts");
    for (i = 0; i < 1025u * 1024u; ++i) {
        require(a[i] == 0, "new page not zeroed");
        a[i] = 0xA55A0000u ^ i;
    }
    for (i = 0; i < 1025u * 1024u; ++i) {
        require(a[i] == (0xA55A0000u ^ i), "pages alias or data changed");
    }
    require(paging_query((uint32_t)a + 123u, &queried) &&
            (queried.physical_address & 4095u) == 123u &&
            queried.flags == PAGING_WRITABLE, "byte translation");
    vmm_test_high_physical = queried.physical_address & ~4095u;
    require(!vmm_free_pages(a + 1) && !vmm_free_pages(a + 1024), "interior free");

    vmm_test_stage = 2;
    /* Exercise borrowed mappings without releasing PMM ownership. Restore the
       VMM-owned second frame before freeing either page. IRQs exclude observers. */
    flags = irq_save();
    require(paging_query((uint32_t)b, &first) &&
            paging_query((uint32_t)b + PAGING_PAGE_SIZE, &second), "two mappings");
    b[0] = 0x12345678u;
    b[1024] = 0xABCDEF01u;
    count = pmm_free_pages();
    require(!paging_map((uint32_t)b, first.physical_address, PAGING_WRITABLE),
            "replacement accepted");
    require(!paging_map(0, first.physical_address, 0) &&
            !paging_map(0x10000u, first.physical_address, 0) &&
            !paging_map(KERNEL_VIRTUAL_END, first.physical_address, 0) &&
            !paging_map((uint32_t)b + 1u, first.physical_address, 0) &&
            !paging_map((uint32_t)b, first.physical_address + 1u, 0) &&
            !paging_map((uint32_t)b, first.physical_address, 4u) &&
            !paging_map((uint32_t)b, 0, 0), "map validation");
    require(!paging_unmap(0x10000u) && !paging_unmap(KERNEL_VIRTUAL_END) &&
            !paging_protect(0x10000u, 0) && !paging_protect((uint32_t)b, 4u),
            "permanent mappings or flags accepted");
    require(paging_unmap((uint32_t)b + PAGING_PAGE_SIZE) &&
            !paging_query((uint32_t)b + PAGING_PAGE_SIZE, &queried) &&
            !paging_unmap((uint32_t)b + PAGING_PAGE_SIZE) &&
            !paging_protect((uint32_t)b + PAGING_PAGE_SIZE, 0), "unmap validation");
    require(paging_map((uint32_t)b + PAGING_PAGE_SIZE, first.physical_address,
                       PAGING_WRITABLE), "borrowed map");
    require(b[1024] == 0x12345678u, "stale TLB after remap");
    require(paging_protect((uint32_t)b + PAGING_PAGE_SIZE, 0) &&
            paging_query((uint32_t)b + PAGING_PAGE_SIZE, &queried) &&
            queried.flags == 0 && b[1024] == b[0], "read-only query/read");
    require(paging_protect((uint32_t)b + PAGING_PAGE_SIZE, PAGING_WRITABLE),
            "restore write permission");
    b[1024] = 0x98765432u;
    require(b[0] == 0x98765432u, "borrowed alias");
    require(paging_unmap((uint32_t)b + PAGING_PAGE_SIZE) &&
            paging_map((uint32_t)b + PAGING_PAGE_SIZE, second.physical_address,
                       PAGING_WRITABLE), "restore owned mapping");
    require(b[1024] == 0xABCDEF01u && pmm_free_pages() == count, "mapping owns frames");
    irq_restore(flags);
    require(vmm_free_pages(a) && !vmm_free_pages(a) && vmm_free_pages(b), "release");
    a = vmm_alloc_pages(1025);
    require((uint32_t)a == paging_identity_end, "virtual reuse");
    for (i = 0; i < 1025u * 1024u; ++i) {
        require(a[i] == 0, "reused allocation not zeroed");
    }
    require(vmm_free_pages(a), "reused release");

    /* Make holes in the PMM's current allocation word, retaining the middle
       frame as an unrelated owner. One virtual allocation must span the gap. */
    for (i = 0; i < 3; ++i) {
        loans[i] = pmm_alloc_page();
        require(loans[i] != 0, "physical fragmentation fixture");
    }
    require(pmm_free_page(loans[0]) && pmm_free_page(loans[2]), "physical holes");
    a = vmm_alloc_pages(3);
    require(a && paging_query((uint32_t)a, &first) &&
            paging_query((uint32_t)a + PAGING_PAGE_SIZE, &second) &&
            second.physical_address != first.physical_address + PAGING_PAGE_SIZE,
            "noncontiguous physical backing");
    for (i = 0; i < 3u * 1024u; ++i) {
        a[i] = 0xF00D0000u ^ i;
    }
    for (i = 0; i < 3u * 1024u; ++i) {
        require(a[i] == (0xF00D0000u ^ i), "fragmented physical data");
    }
    require(vmm_free_pages(a) && pmm_free_page(loans[1]), "physical fixture release");

    vmm_test_stage = 3;
    /* On 128 MiB this must map a large partial run, hit physical exhaustion,
       and roll everything back. On 512 MiB virtual exhaustion comes first. */
    if (physical_before < virtual_before) {
        require(vmm_alloc_pages(physical_before + 1u) == 0, "physical exhaustion");
        require(pmm_free_pages() == physical_before &&
                vmm_free_page_count() == virtual_before, "partial rollback counts");
        for (i = paging_identity_end; i < KERNEL_VIRTUAL_END; i += PAGING_PAGE_SIZE) {
            require(!paging_query(i, &queried), "partial rollback left mapping");
        }
    }
    count = physical_before < virtual_before ? physical_before : virtual_before;
    a = vmm_alloc_pages(count);
    require(a != 0 && vmm_alloc_pages(1) == 0, "exhaustion or largest allocation");
    require(vmm_free_pages(a), "largest release");

    if (physical_before >= virtual_before) {
        /* Two non-adjacent holes have enough total pages, but no fitting run. */
        a = vmm_alloc_pages(1);
        b = vmm_alloc_pages(1);
        c = vmm_alloc_pages(virtual_before - 2u);
        require(a && b && c && vmm_free_pages(a) && vmm_free_pages(c), "fragment setup");
        require(vmm_alloc_pages(virtual_before - 1u) == 0, "fragmentation ignored");
        require(vmm_free_pages(b), "fragment release");
        a = vmm_alloc_pages(virtual_before);
        require(a != 0 && vmm_free_pages(a), "adjacent holes did not merge");
    }

    vmm_test_stage = 4;
    flags = irq_save();
    a = vmm_alloc_pages(1);
    require(a && (read_flags() & 0x200u) == 0 && vmm_free_pages(a) &&
            (read_flags() & 0x200u) == 0, "disabled IF not preserved");
    irq_restore(flags);
    require((read_flags() & 0x200u) == (original_flags & 0x200u), "enabled IF not preserved");
    require(pmm_free_pages() == physical_before &&
            vmm_free_page_count() == virtual_before, "final counts");
    for (i = paging_identity_end; i < KERNEL_VIRTUAL_END; i += PAGING_PAGE_SIZE) {
        require(!paging_query(i, &queried), "leaked mapping");
    }
    vmm_test_stage = 5;
    kprintf("VMM tests passed: mapping/reuse/permissions/exhaustion/rollback/IF\n");
}

/* The harness redirects a disposable, fully booted guest here with a fresh
   stack. Returning to an interrupted idle-loop frame would violate its ABI. */
void vmm_test_entry(void)
{
    extern void interrupts_enable_and_halt(void);
    vmm_self_test();
    for (;;) {
        interrupts_enable_and_halt();
    }
}

/* Each fault probe runs in its own disposable guest and must never return. */
void vmm_fault_null(void)
{
    vmm_test_fault_address = 0;
    __asm__ volatile ("movl $1, 0" : : : "memory");
}

void vmm_fault_freed(void)
{
    volatile uint32_t *page = vmm_alloc_pages(1);
    require(page != 0, "fault allocation");
    *page = 123;
    vmm_test_fault_address = (uint32_t)page;
    require(vmm_free_pages((void *)page), "fault free");
    *page = 456;
}

void vmm_fault_readonly(void)
{
    volatile uint32_t *page = vmm_alloc_pages(1);
    require(page != 0, "read-only allocation");
    *page = 123; /* Populate the writable TLB entry before revoking permission. */
    vmm_test_fault_address = (uint32_t)page;
    require(paging_protect((uint32_t)page, 0), "fault protect");
    *page = 456;
}

void vmm_fault_boundary(void)
{
    vmm_test_fault_address = KERNEL_VIRTUAL_END;
    *(volatile uint32_t *)KERNEL_VIRTUAL_END = 1;
}
