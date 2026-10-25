/*
 * Guest-side heap contract tests, including pressure from real PMM/VMM limits.
 * Expected invalid frees run separately so a deliberate panic cannot hide leaks.
 */

#include "kheap.h"
#include "kernel_test.h"
#include "vmm.h"
#include "pmm.h"
#include "irq.h"
#include "panic.h"
#include "conio.h"

#define SLOT_COUNT 32u

volatile uint32_t kheap_test_stage;
volatile uint32_t kheap_test_high_physical;
volatile uint32_t kheap_test_fault_address;
volatile uint32_t kheap_test_require_high;
static void *slots[SLOT_COUNT];
static uint32_t sizes[SLOT_COUNT];
static uint8_t tags[SLOT_COUNT];

static void require(bool condition, const char *reason)
{
    if (!condition) {
        kernel_panic("Heap test", reason);
    }
}

static uint32_t read_flags(void)
{
    uint32_t flags;
    __asm__ volatile ("pushfl; popl %0" : "=r" (flags));
    return flags;
}

static void fill(void *pointer, uint32_t size, uint8_t tag)
{
    volatile uint8_t *bytes = pointer;
    uint32_t i;
    for (i = 0; i < size; ++i) {
        bytes[i] = (uint8_t)(tag ^ (i * 13u + i / 251u));
    }
}

static void check(void *pointer, uint32_t size, uint8_t tag)
{
    volatile const uint8_t *bytes = pointer;
    uint32_t i;
    for (i = 0; i < size; ++i) {
        require(bytes[i] == (uint8_t)(tag ^ (i * 13u + i / 251u)), "payload overwritten");
    }
}

static void require_empty(uint32_t physical, uint32_t virtual)
{
    require(kheap_live_allocations() == 0 && kheap_allocated_pages() == 0,
            "empty heap retains ownership");
    require(pmm_free_pages() == physical && vmm_free_page_count() == virtual,
            "pages leaked to heap");
}

void kheap_self_test(void)
{
    static const uint32_t boundary_sizes[] = {1, 15, 16, 17, 31, 32, 33, 255,
                                              4095, 4096, 4097, 100000};
    uint32_t physical_before = pmm_free_pages();
    uint32_t virtual_before = vmm_free_page_count();
    uint32_t original_if = read_flags() & 0x200u;
    uint32_t i;
    uint32_t j;
    uint32_t saved;
    uint32_t pages;
    uint32_t allocated;
    uint32_t physical;
    uint32_t virtual;
    uint32_t random = 0x6D2B79F5u;
    void *anchor;
    void *payload;
    void *pressure;
    void *recycled;
    void *neighbor;
    PagingMapping mapping;

    clear_screen();
    kheap_test_stage = 1;
    require_empty(physical_before, virtual_before);
    kfree(0);
    require(kmalloc(0) == 0 && kmalloc(0xFFFFFFFFu) == 0 && kmalloc(0xFFFFFFF0u) == 0 &&
            kmalloc(0xFFFFEFE0u) == 0, "zero/overflow allocation");
    require(kcalloc(0, 1) == 0 && kcalloc(1, 0) == 0 &&
            kcalloc(0x80000001u, 2) == 0 && kcalloc(0xFFFFFFFFu, 0xFFFFFFFFu) == 0,
            "calloc multiplication overflow");
    require_empty(physical_before, virtual_before);

    /* Simultaneous, differently sized payloads expose overlap, incorrect
       rounding, and page-boundary errors without looking at private headers. */
    for (i = 0; i < sizeof(boundary_sizes) / sizeof(boundary_sizes[0]); ++i) {
        sizes[i] = boundary_sizes[i];
        tags[i] = (uint8_t)(i + 31u);
        slots[i] = kmalloc(sizes[i]);
        require(slots[i] != 0 && ((uint32_t)slots[i] & (KHEAP_ALIGNMENT - 1u)) == 0,
                "payload alignment/allocation");
        for (j = 0; j < i; ++j) {
            require((uint32_t)slots[i] + sizes[i] <= (uint32_t)slots[j] ||
                    (uint32_t)slots[j] + sizes[j] <= (uint32_t)slots[i], "payload overlap");
        }
        fill(slots[i], sizes[i], tags[i]);
    }
    require(paging_query((uint32_t)slots[0], &mapping), "heap mapping missing");
    kheap_test_high_physical = mapping.physical_address;
    if (kheap_test_require_high) {
        require(mapping.physical_address >= 0x10000000u, "expected high backing");
        /* The controlled firmware map reserves a hole after the first high
           frame. A larger heap payload must span independent physical frames. */
        bool gap = false;
        uint32_t previous = 0;
        uint32_t start = (uint32_t)slots[11] & ~(PAGE_SIZE - 1u);
        for (i = start; i < (uint32_t)slots[11] + sizes[11]; i += PAGE_SIZE) {
            require(paging_query(i, &mapping) && mapping.physical_address >= 0x10000000u,
                    "large high mapping");
            gap = gap || (previous != 0 && mapping.physical_address != previous + PAGE_SIZE);
            previous = mapping.physical_address;
        }
        require(gap, "controlled high backing did not cross a reserved hole");
    }
    for (i = 0; i < 12; ++i) {
        check(slots[i], sizes[i], tags[i]);
    }
    /* Alternate frees leave both live and free neighbors while extents unlink. */
    for (i = 0; i < 12; i += 2) {
        kfree(slots[i]);
        slots[i] = 0;
    }
    for (i = 1; i < 12; i += 2) {
        check(slots[i], sizes[i], tags[i]);
        kfree(slots[i]);
        slots[i] = 0;
    }
    require_empty(physical_before, virtual_before);

    kheap_test_stage = 2;
    /* Seven 2 KiB payloads fill one small extent closely enough that a 4 KiB
       request fits only if adjacent holes really coalesce. Keep live neighbors
       so passing cannot be explained by releasing and regrowing the extent. */
    for (i = 0; i < 7; ++i) {
        slots[i] = kmalloc(2048);
        require(slots[i] != 0, "coalescing fixture");
        fill(slots[i], 2048, (uint8_t)i);
    }
    pages = kheap_allocated_pages();
    kfree(slots[1]);
    kfree(slots[2]);
    payload = kmalloc(4096);
    require(payload == slots[1] && kheap_allocated_pages() == pages, "left coalescing");
    kfree(payload);
    kfree(slots[0]);
    payload = kmalloc(6000);
    require(payload == slots[0] && kheap_allocated_pages() == pages, "right coalescing");
    for (i = 3; i < 7; ++i) {
        check(slots[i], 2048, (uint8_t)i);
    }
    kfree(payload);
    for (i = 3; i < 7; ++i) {
        kfree(slots[i]);
    }
    for (i = 0; i < SLOT_COUNT; ++i) {
        slots[i] = 0;
    }
    require_empty(physical_before, virtual_before);

    /* calloc must clear recycled blocks, not just newly mapped VMM pages. */
    anchor = kmalloc(32);
    payload = kmalloc(333);
    neighbor = kmalloc(64);
    require(anchor && payload && neighbor, "calloc fixture");
    fill(anchor, 32, 0x11u);
    fill(neighbor, 64, 0xBBu);
    fill(payload, 333, 0xCCu);
    recycled = payload;
    kfree(payload);
    payload = kcalloc(37, 9);
    require(payload == recycled, "calloc reuse");
    for (i = 0; i < 333; ++i) {
        require(((volatile uint8_t *)payload)[i] == 0, "calloc did not clear reuse");
    }
    check(anchor, 32, 0x11u);
    check(neighbor, 64, 0xBBu);
    kfree(payload);
    kfree(neighbor);
    kfree(anchor);
    require_empty(physical_before, virtual_before);
    payload = kcalloc(4097, 3);
    require(payload != 0, "multi-page calloc");
    for (i = 0; i < 4097u * 3u; ++i) {
        require(((volatile uint8_t *)payload)[i] == 0, "multi-page calloc not zeroed");
    }
    kfree(payload);
    require_empty(physical_before, virtual_before);

    /* An unrelated VMM allocation separates two heap extents. Releasing one
       extent must neither unmap that allocation nor damage the other extent. */
    anchor = kmalloc(64);
    pressure = vmm_alloc_pages(3);
    payload = kmalloc(20000);
    require(anchor && pressure && payload, "disjoint extent growth");
    fill(pressure, 3u * PAGE_SIZE, 0xD3u);
    fill(payload, 20000, 0x71u);
    kfree(anchor);
    check(payload, 20000, 0x71u);
    check(pressure, 3u * PAGE_SIZE, 0xD3u);
    kfree(payload);
    check(pressure, 3u * PAGE_SIZE, 0xD3u);
    require(kheap_allocated_pages() == 0 && kheap_live_allocations() == 0,
            "disjoint extents were not reclaimed");
    require(vmm_free_pages(pressure), "disjoint barrier release");
    require_empty(physical_before, virtual_before);

    kheap_test_stage = 3;
    /* Deterministic xorshift mixing exercises many list shapes; it is not a
       cryptographic PRNG. Check data in all survivors throughout the sequence. */
    for (i = 0; i < 600; ++i) {
        uint32_t slot;
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        slot = random % SLOT_COUNT;
        if (slots[slot] != 0) {
            check(slots[slot], sizes[slot], tags[slot]);
            kfree(slots[slot]);
            slots[slot] = 0;
        } else {
            sizes[slot] = 1u + random % ((i & 7u) == 0 ? 12000u : 1024u);
            tags[slot] = (uint8_t)(random >> 24);
            slots[slot] = kmalloc(sizes[slot]);
            require(slots[slot] != 0, "mixed allocation");
            fill(slots[slot], sizes[slot], tags[slot]);
        }
        if (i % 17u == 0) {
            for (j = 0; j < SLOT_COUNT; ++j) {
                if (slots[j] != 0) {
                    check(slots[j], sizes[j], tags[j]);
                }
            }
        }
    }
    for (i = 0; i < SLOT_COUNT; ++i) {
        if (slots[i] != 0) {
            check(slots[i], sizes[i], tags[i]);
            kfree(slots[i]);
            slots[i] = 0;
        }
    }
    require_empty(physical_before, virtual_before);

    kheap_test_stage = 4;
    anchor = kmalloc(257);
    require(anchor != 0, "pressure anchor");
    fill(anchor, 257, 0x55u);
    pages = kheap_allocated_pages();
    allocated = kheap_live_allocations();
    physical = pmm_free_pages();
    virtual = vmm_free_page_count();
    /* This payload alone consumes every remaining page; headers require extra
       backing. In low RAM VMM must undo a large partial mapping on failure. */
    i = physical < virtual ? physical : virtual;
    require(kmalloc(i * PAGE_SIZE) == 0, "oversized backing request succeeded");
    require(kheap_allocated_pages() == pages && kheap_live_allocations() == allocated &&
            pmm_free_pages() == physical && vmm_free_page_count() == virtual,
            "failed growth changed ownership");
    check(anchor, 257, 0x55u);
    pressure = vmm_alloc_pages(i);
    require(pressure != 0, "pressure VMM reservation");
    payload = kmalloc(128);
    require(payload != 0, "cannot reuse a block without new pages");
    fill(payload, 128, 0xA5u);
    require(kmalloc(20000) == 0 && kcalloc(5000, 4) == 0, "exhaustion not reported");
    check(anchor, 257, 0x55u);
    check(payload, 128, 0xA5u);
    kfree(payload);
    require(vmm_free_pages(pressure), "pressure release");
    payload = kmalloc(20000);
    require(payload != 0, "heap did not recover after exhaustion");
    kfree(payload);
    kfree(anchor);
    require_empty(physical_before, virtual_before);

    /* Fill almost all of a small extent, then leave only one backing page
       available. A four-page growth preference must not waste that last page. */
    anchor = kmalloc(12000);
    recycled = kmalloc(4000);
    require(anchor && recycled, "minimal growth fixture");
    pages = kheap_allocated_pages();
    physical = pmm_free_pages();
    virtual = vmm_free_page_count();
    i = physical < virtual ? physical : virtual;
    require(i > 1, "minimal growth free pool");
    pressure = vmm_alloc_pages(i - 1u);
    require(pressure != 0, "minimal growth pressure");
    payload = kmalloc(1024);
    require(payload != 0 && kheap_allocated_pages() == pages + 1u,
            "one-page growth fallback");
    fill(payload, 1024, 0xE7u);
    check(payload, 1024, 0xE7u);
    kfree(payload);
    require(vmm_free_pages(pressure), "minimal growth pressure release");
    kfree(recycled);
    kfree(anchor);
    require_empty(physical_before, virtual_before);

    saved = irq_save();
    payload = kcalloc(3, 17);
    require(payload && (read_flags() & 0x200u) == 0, "allocation enabled IRQs");
    kfree(payload);
    require((read_flags() & 0x200u) == 0, "free enabled IRQs");
    irq_restore(saved);
    require((read_flags() & 0x200u) == original_if, "caller IF changed");
    require_empty(physical_before, virtual_before);
    kprintf("Heap tests passed: alignment/data/split/coalesce/calloc/pressure/IF\n");
    kheap_test_stage = 5;
}

static __attribute__((noreturn)) void test_idle(void)
{
    extern void interrupts_enable_and_halt(void);
    for (;;) {
        interrupts_enable_and_halt();
    }
}

void kheap_test_entry(void)
{
    kheap_self_test();
    test_idle();
}

void kheap_test_high_entry(void)
{
    kheap_test_require_high = 1;
    kheap_test_entry();
}

void kheap_test_preinit_entry(void)
{
    require(kmalloc(1) == 0 && kcalloc(1, 1) == 0, "allocation before initialization");
    kfree(0);
    require(kheap_allocated_pages() == 0 && kheap_live_allocations() == 0, "preinit ownership");
    kheap_init();
    kprintf("Heap tests passed: preinit contract\n");
    kheap_test_stage = 5;
    test_idle();
}

/* Fresh guests isolate contract violations from the positive ownership tests. */
void kheap_bad_interior(void)
{
    uint8_t *payload = kmalloc(128);
    require(payload != 0, "bad-free fixture");
    kfree(payload + KHEAP_ALIGNMENT);
}

void kheap_bad_unaligned(void)
{
    uint8_t *payload = kmalloc(128);
    require(payload != 0, "unaligned fixture");
    kfree(payload + 1);
}

void kheap_bad_double(void)
{
    void *anchor = kmalloc(32);
    void *payload = kmalloc(128);
    require(anchor && payload, "double-free fixture");
    kfree(payload);
    kfree(payload);
}

void kheap_bad_foreign(void)
{
    void *foreign = vmm_alloc_pages(1);
    require(foreign != 0, "foreign VMM fixture");
    kfree(foreign);
}

void kheap_bad_stale(void)
{
    void *payload = kmalloc(20000);
    require(payload != 0, "stale free fixture");
    kfree(payload); /* Its entire extent is now unmapped. */
    kfree(payload); /* Must diagnose the pointer without reading freed metadata. */
}

void kheap_bad_unmapped(void)
{
    kfree((void *)KERNEL_VIRTUAL_END);
}

void kheap_bad_reinit(void)
{
    kheap_init();
}

void kheap_fault_released(void)
{
    volatile uint8_t *payload = kmalloc(20000);
    require(payload != 0, "released extent fixture");
    *payload = 42;
    kheap_test_fault_address = (uint32_t)payload;
    kfree((void *)payload);
    *payload = 43;
}
