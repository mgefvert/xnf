/*
 * Disposable-guest tests for object type, namespace, lifetime and heap pressure.
 * Test bodies own an auxiliary allocation so destruction exercises cleanup.
 */
#include "object.h"
#include "kheap.h"
#include "kernel_test.h"
#include "panic.h"
#include "conio.h"
#include "pmm.h"
#include "vmm.h"
#include "irq.h"

typedef struct TestBody { uint32_t marker; void *resource; } TestBody;
static uint32_t destroyed;
static void *pressure_slots[512];
volatile uint32_t object_test_stage;

static void require(bool condition, const char *reason)
{
    if (!condition) kernel_panic("Object test", reason);
}

static uint32_t read_flags(void)
{
    uint32_t flags;
    __asm__ volatile ("pushfl; popl %0" : "=r" (flags));
    return flags;
}

static void test_destroy(void *body)
{
    TestBody *test = body;
    ++destroyed;
    kfree(test->resource);
}

/* Identical display names still denote distinct types. */
static const ObjectType test_type = {"Test device", sizeof(TestBody), test_destroy};
static const ObjectType other_type = {"Test device", sizeof(TestBody), test_destroy};
static const ObjectType invalid_type = {"Invalid", 0, 0};
static const ObjectType huge_type = {"Huge", 0xFFFFFFFFu, 0};
static const ObjectType null_name_type = {0, sizeof(TestBody), 0};
static const ObjectType empty_name_type = {"", sizeof(TestBody), 0};
static const ObjectType control_name_type = {"bad\nname", sizeof(TestBody), 0};

void object_self_test(void)
{
    uint32_t before_allocations = kheap_live_allocations(), before_pages = kheap_allocated_pages();
    uint32_t before_physical = pmm_free_pages(), before_virtual = vmm_free_page_count();
    uint32_t count, i, available, allocations, saved;
    Object *first, *second, *found, *alias, *sentinel, *pressure_object;
    TestBody *body;
    void *pressure;
    char name[] = "/device/console0";
    char long_name[OBJECT_NAME_MAX + 2u];
    char type_name[OBJECT_TYPE_NAME_MAX + 2u];
    ObjectType bounded_type = {type_name, 1u, 0};

    object_test_stage = 1;
    require(object_manager_init() == OBJECT_ALREADY_INITIALIZED, "repeated initialization");
    sentinel = (Object *)1;
    require(object_create(&invalid_type, &sentinel) == OBJECT_INVALID && sentinel == (Object *)1 &&
            object_create(&huge_type, &sentinel) == OBJECT_NO_MEMORY && sentinel == (Object *)1 &&
            object_create(0, &sentinel) == OBJECT_INVALID && object_create(&test_type, 0) == OBJECT_INVALID &&
            object_create(&null_name_type, &sentinel) == OBJECT_INVALID &&
            object_create(&empty_name_type, &sentinel) == OBJECT_INVALID &&
            object_create(&control_name_type, &sentinel) == OBJECT_INVALID && sentinel == (Object *)1 &&
            object_type(0) == 0,
            "invalid type/create rollback");
    for (i = 0; i < OBJECT_TYPE_NAME_MAX; ++i) type_name[i] = 'x';
    type_name[OBJECT_TYPE_NAME_MAX] = 0;
    require(object_create(&bounded_type, &first) == OBJECT_OK &&
            object_type(first) == &bounded_type, "type name maximum length");
    object_release(first);
    type_name[OBJECT_TYPE_NAME_MAX] = 'x';
    type_name[OBJECT_TYPE_NAME_MAX + 1u] = 0;
    require(object_create(&bounded_type, &sentinel) == OBJECT_INVALID && sentinel == (Object *)1,
            "overlong type name");
    require(object_create(&test_type, &first) == OBJECT_OK, "first create");
    body = object_body(first, &test_type);
    require(body && ((uint32_t)body & (KHEAP_ALIGNMENT - 1u)) == 0 && !body->marker && !body->resource &&
            !object_body(first, &other_type) && object_type(first) == &test_type &&
            kstrcmp(object_type(first)->name, "Test device") == 0, "body alignment/zero/type");
    body->marker = 42;
    body->resource = kmalloc(64);
    require(body->resource != 0, "body resource");
    require(object_create(&other_type, &second) == OBJECT_OK, "second create");
    require(object_publish(STRING_VIEW_LITERAL("device/no-leading-slash"), first) == OBJECT_INVALID &&
            object_publish(STRING_VIEW_LITERAL("/device//x"), first) == OBJECT_INVALID &&
            object_publish(STRING_VIEW_LITERAL("/device/../x"), first) == OBJECT_INVALID &&
            object_publish(STRING_VIEW_LITERAL("/device/."), first) == OBJECT_INVALID &&
            object_publish(STRING_VIEW_LITERAL("/device/"), first) == OBJECT_INVALID &&
            object_publish(string_view("/a\0b", 4), first) == OBJECT_INVALID,
            "invalid names");
    long_name[0] = '/';
    for (i = 1; i < sizeof(long_name); ++i) long_name[i] = 'x';
    require(object_publish(string_view(long_name, OBJECT_NAME_MAX + 1u), first) == OBJECT_INVALID &&
            object_publish(string_view(long_name, OBJECT_NAME_MAX), first) == OBJECT_OK &&
            object_unpublish(string_view(long_name, OBJECT_NAME_MAX), first) == OBJECT_OK,
            "name length boundary");
    require(object_publish(string_view_cstr(name), first) == OBJECT_OK &&
            object_publish(STRING_VIEW_LITERAL("/device/console0"), second) == OBJECT_NAME_EXISTS &&
            object_publish(STRING_VIEW_LITERAL("/alias/console0"), first) == OBJECT_OK,
            "publish duplicate/alias");
    name[1] = 'D'; /* The registry has its own immutable copy. */
    require(object_lookup(STRING_VIEW_LITERAL("/device/console0"), &other_type, &sentinel) == OBJECT_TYPE_MISMATCH &&
            sentinel == (Object *)1 &&
            object_lookup(string_view_cstr(name), &test_type, &sentinel) == OBJECT_NOT_FOUND && sentinel == (Object *)1,
            "type/name lookup rollback");
    object_release(first); /* Only two registry references remain. */
    require(object_lookup(STRING_VIEW_LITERAL("/device/console0"), &test_type, &found) == OBJECT_OK &&
            ((TestBody *)object_body(found, &test_type))->marker == 42 &&
            object_type(found) == &test_type &&
            object_lookup(STRING_VIEW_LITERAL("/alias/console0"), &test_type, &alias) == OBJECT_OK && alias == found,
            "lookup retains object");
    require(object_retain(found) == OBJECT_OK, "retain existing reference");
    object_release(found);
    require(object_unpublish(STRING_VIEW_LITERAL("/device/console0"), second) == OBJECT_WRONG_OBJECT &&
            object_unpublish(STRING_VIEW_LITERAL("/device/console0"), found) == OBJECT_OK &&
            object_lookup(STRING_VIEW_LITERAL("/device/console0"), &test_type, &sentinel) == OBJECT_NOT_FOUND &&
            ((TestBody *)object_body(found, &test_type))->marker == 42 && !destroyed,
            "unpublish keeps borrowed references live");
    require(object_publish(STRING_VIEW_LITERAL("/device/console0"), second) == OBJECT_OK &&
            object_unpublish(STRING_VIEW_LITERAL("/device/console0"), found) == OBJECT_WRONG_OBJECT &&
            object_unpublish(STRING_VIEW_LITERAL("/alias/console0"), alias) == OBJECT_OK,
            "name reuse protects replacement");
    object_release(found);
    require(!destroyed, "premature destruction");
    object_release(alias);
    require(destroyed == 1, "last release destruction");
    require(object_unpublish(STRING_VIEW_LITERAL("/device/console0"), second) == OBJECT_OK &&
            object_unpublish(STRING_VIEW_LITERAL("/device/console0"), second) == OBJECT_NOT_FOUND,
            "unpublish replacement");
    object_release(second);
    require(destroyed == 2 && kheap_live_allocations() == before_allocations &&
            kheap_allocated_pages() == before_pages, "object and registry cleanup");

    require(object_create(&test_type, &pressure_object) == OBJECT_OK &&
            object_publish(STRING_VIEW_LITERAL("/device/existing"), pressure_object) == OBJECT_OK,
            "pressure object");
    available = pmm_free_pages();
    if (available > vmm_free_page_count()) available = vmm_free_page_count();
    pressure = vmm_alloc_pages(available);
    require(pressure != 0, "pressure reservation");
    count = 0;
    while (count < 512 && (pressure_slots[count] = kmalloc(32)) != 0) ++count;
    require(count < 512, "pressure slot limit");
    allocations = kheap_live_allocations();
    require(object_create(&test_type, &sentinel) == OBJECT_NO_MEMORY && sentinel == (Object *)1 &&
            object_publish(STRING_VIEW_LITERAL("/device/exhaustion"), pressure_object) == OBJECT_NO_MEMORY &&
            object_lookup(STRING_VIEW_LITERAL("/device/exhaustion"), &test_type, &sentinel) == OBJECT_NOT_FOUND &&
            object_lookup(STRING_VIEW_LITERAL("/device/existing"), &test_type, &found) == OBJECT_OK &&
            found == pressure_object && kheap_live_allocations() == allocations && destroyed == 2,
            "existing registry allocation failure rollback");
    object_release(found);
    require(object_unpublish(STRING_VIEW_LITERAL("/device/existing"), pressure_object) == OBJECT_OK,
            "remove pressure registry");
    /* Consume holes released by the last registry entry, then test first
       publication separately, including cleanup of a temporary dictionary. */
    while (count < 512 && (pressure_slots[count] = kmalloc(32)) != 0) ++count;
    require(count < 512, "second pressure slot limit");
    allocations = kheap_live_allocations();
    require(object_publish(STRING_VIEW_LITERAL("/device/exhaustion"), pressure_object) == OBJECT_NO_MEMORY &&
            object_lookup(STRING_VIEW_LITERAL("/device/exhaustion"), &test_type, &sentinel) == OBJECT_NOT_FOUND &&
            kheap_live_allocations() == allocations && destroyed == 2, "first publication failure rollback");
    while (count) kfree(pressure_slots[--count]);
    require(vmm_free_pages(pressure), "release pressure");
    require(object_publish(STRING_VIEW_LITERAL("/device/exhaustion"), pressure_object) == OBJECT_OK &&
            object_unpublish(STRING_VIEW_LITERAL("/device/exhaustion"), pressure_object) == OBJECT_OK,
            "publish recovers after pressure");
    object_release(pressure_object);
    saved = irq_save();
    require(object_create(&test_type, &first) == OBJECT_OK &&
            object_publish(STRING_VIEW_LITERAL("/device/if"), first) == OBJECT_OK &&
            object_unpublish(STRING_VIEW_LITERAL("/device/if"), first) == OBJECT_OK &&
            (read_flags() & 0x200u) == 0, "operations enabled IRQs");
    object_release(first);
    require((read_flags() & 0x200u) == 0, "destruction enabled IRQs");
    irq_restore(saved);
    require(object_create(&test_type, &first) == OBJECT_OK &&
            object_publish(STRING_VIEW_LITERAL("/saturation"), first) == OBJECT_OK,
            "reference saturation fixture");
    object_test_force_references(first, 0xFFFFFFFFu);
    sentinel = (Object *)1;
    require(object_retain(first) == OBJECT_REFERENCE_LIMIT &&
            object_publish(STRING_VIEW_LITERAL("/saturation2"), first) == OBJECT_REFERENCE_LIMIT &&
            object_lookup(STRING_VIEW_LITERAL("/saturation"), &test_type, &sentinel) == OBJECT_REFERENCE_LIMIT &&
            sentinel == (Object *)1, "reference overflow rollback");
    object_test_force_references(first, 2); /* One caller and one registry reference. */
    require(object_unpublish(STRING_VIEW_LITERAL("/saturation"), first) == OBJECT_OK,
            "saturation fixture unpublish");
    object_release(first);
    require(destroyed == 5 && kheap_live_allocations() == before_allocations &&
            kheap_allocated_pages() == before_pages && pmm_free_pages() == before_physical &&
            vmm_free_page_count() == before_virtual, "complete ownership cleanup");
    kputs("Object tests passed: names/types/references/pressure\n");
    object_test_stage = 2;
}

static __attribute__((noreturn)) void test_idle(void)
{
    extern void interrupts_enable_and_halt(void);
    for (;;) interrupts_enable_and_halt();
}

void object_test_entry(void)
{
    object_self_test();
    test_idle();
}

/* Selected at the kheap_init boundary in a fresh guest. */
void object_test_preinit_entry(void)
{
    Object *output = (Object *)1;
    require(!kheap_is_initialized() && object_manager_init() == OBJECT_NOT_READY &&
            object_create(&test_type, &output) == OBJECT_NOT_READY && output == (Object *)1 &&
            object_type(0) == 0,
            "pre-heap initialization");
    kheap_init();
    require(object_manager_init() == OBJECT_OK && object_manager_init() == OBJECT_ALREADY_INITIALIZED,
            "initialization after heap");
    kputs("Object tests passed: initialization\n");
    object_test_stage = 2;
    test_idle();
}

void object_test_bad_release(void)
{
    Object *object;
    require(object_create(&test_type, &object) == OBJECT_OK &&
            object_publish(STRING_VIEW_LITERAL("/bad"), object) == OBJECT_OK, "bad release fixture");
    object_release(object);
    object_release(object); /* Only the registry owns a reference now. */
}
