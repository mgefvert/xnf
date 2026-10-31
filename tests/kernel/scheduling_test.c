/*
 * Concurrent service, preemption guard, sleeping, and idle regression fixture.
 * A disposable guest runs these threads; shared test counters are single-writer.
 */
#include "types.h"
#include "scheduler.h"
#include "preempt.h"
#include "irq.h"
#include "kheap.h"
#include "vmm.h"
#include "object.h"
#include "conio.h"

extern volatile uint32_t timer_ticks;
extern volatile uint32_t scheduler_test_stage, scheduler_test_error;
extern volatile uint32_t scheduler_test_counts[2];
static Object *shared;
static volatile uint32_t destroyed;

static void destroy_test(void *body)
{
    uint32_t flags;
    (void)body;
    /* A callback may block; the object manager must release its internal
       preemption guard before entering a destructor. */
    if (preempt_disabled() || !scheduler_sleep(1)) scheduler_test_error = 30;
    flags = irq_save();
    ++destroyed;
    irq_restore(flags);
}
static const ObjectType test_type = {"scheduler test", 4, destroy_test};

static void service_worker(uint32_t id)
{
    uint32_t i;
    StringView name = id ? STRING_VIEW_LITERAL("/thread/b") : STRING_VIEW_LITERAL("/thread/a");
    for (i = 0; i < 8; ++i) {
        Object *object, *found;
        uint32_t *a = kmalloc(128), *b = kcalloc(32, sizeof(uint32_t));
        uint32_t *page = vmm_alloc_pages(1);
        if (!a || !b || !page || b[0] || b[31] || page[0]) {
            scheduler_test_error = 31; break;
        }
        a[0] = page[0] = id * 100u + i;
        if (object_create(&test_type, &object) != OBJECT_OK ||
            object_publish(name, object) != OBJECT_OK ||
            object_lookup(name, &test_type, &found) != OBJECT_OK) {
            scheduler_test_error = 32; break;
        }
        object_release(found);
        if (object_unpublish(name, object) != OBJECT_OK ||
            object_lookup(STRING_VIEW_LITERAL("/thread/shared"), &test_type, &found) != OBJECT_OK) {
            scheduler_test_error = 33; break;
        }
        object_release(found);
        object_release(object); /* Destructor sleeps outside the object guard. */
        if (a[0] != id * 100u + i || page[0] != a[0] || !vmm_free_pages(page))
            scheduler_test_error = 34;
        kfree(b); kfree(a);
        kputs(id ? "BBBBBBBBBB\n" : "AAAAAAAAAA\n");
        ++scheduler_test_counts[id];
    }
    /* Both workers remain asleep beyond the bootstrap's deadline. */
    if (!scheduler_sleep(300)) scheduler_test_error = 35;
    for (;;) { }
}
static void service_first(void) { service_worker(0); }
static void service_second(void) { service_worker(1); }

void scheduler_test_syncsleep(void)
{
    uint32_t flags, before_a, before_b, start;
    (void)irq_save();
    if (scheduler_sleep(1) || object_create(&test_type, &shared) != OBJECT_OK ||
        object_publish(STRING_VIEW_LITERAL("/thread/shared"), shared) != OBJECT_OK)
        scheduler_test_error = 36;
    clear_screen();
    /* Exercise sleep deadlines over counter wrap in this disposable guest. */
    timer_ticks = 0xFFFFFFF0u;
    __asm__ volatile ("sti" : : : "memory");
    if (!scheduler_start(service_first, service_second)) scheduler_test_error = 37;

    preempt_disable();
    preempt_disable();
    before_a = scheduler_test_counts[0]; before_b = scheduler_test_counts[1];
    start = timer_ticks;
    while ((uint32_t)(timer_ticks - start) < 4u) { }
    preempt_enable();
    if (scheduler_test_counts[0] != before_a || scheduler_test_counts[1] != before_b ||
        scheduler_sleep(1) || !preempt_disabled()) scheduler_test_error = 38;
    preempt_enable();
    flags = irq_save();
    preempt_disable(); preempt_enable();
    {
        uint32_t current_flags;
        __asm__ volatile ("pushfl; popl %0" : "=r" (current_flags));
        if ((current_flags & 0x200u) || scheduler_sleep(1)) scheduler_test_error = 39;
    }
    irq_restore(flags);
    if (scheduler_sleep(0) || scheduler_sleep(0x80000000u)) scheduler_test_error = 40;
    scheduler_test_stage = 4;
    start = timer_ticks;
    if (!scheduler_sleep(70) || (uint32_t)(timer_ticks - start) < 70u ||
        scheduler_test_counts[0] != 8 || scheduler_test_counts[1] != 8 || destroyed != 16)
        scheduler_test_error = 41;
    if (object_unpublish(STRING_VIEW_LITERAL("/thread/shared"), shared) != OBJECT_OK)
        scheduler_test_error = 42;
    object_release(shared);
    scheduler_test_stage = 5;
    for (;;) { }
}
