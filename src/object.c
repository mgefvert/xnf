/*
 * Typed kernel object lifetime and a lazily allocated named-object registry.
 * Registry entries hold references; the heap owns object headers and bodies.
 */
#include "object.h"
#include "kheap.h"
#include "panic.h"

#define OBJECT_MAX_U32 (~(uint32_t)0)

struct Object {
    const ObjectType *type;
    uint32_t references;
    uint32_t publications;
    uint32_t reserved;
};

/* The body follows the header in the same allocation. A whole alignment unit
   preserves kmalloc's 16-byte alignment for any supported kernel body. */
_Static_assert(sizeof(Object) == KHEAP_ALIGNMENT, "object body alignment");

static bool manager_initialized;
static Dictionary *registry;

#ifdef XNF_BUILD_TESTS
/* Reach the saturation boundary without billions of retains. This fixture is
   absent from normal kernels and is used only with a disposable test object. */
void object_test_force_references(Object *object, uint32_t references)
{
    object->references = references;
}
#endif

ObjectResult object_manager_init(void)
{
    if (manager_initialized) return OBJECT_ALREADY_INITIALIZED;
    if (!kheap_is_initialized()) return OBJECT_NOT_READY;
    /* No allocation: early boot and synthetic PMM fixtures need no heap pages. */
    manager_initialized = true;
    return OBJECT_OK;
}

/* Names are flat, case-sensitive qualified ASCII names. Slash separates
   nonempty components; dot and dot-dot are reserved to avoid future ambiguity. */
static bool valid_name(StringView name)
{
    uint32_t i, start = 1;
    if (!name.data || name.length < 2u || name.length > OBJECT_NAME_MAX || name.data[0] != '/') return false;
    for (i = 1; i <= name.length; ++i) {
        uint8_t c = i == name.length ? '/' : (uint8_t)name.data[i];
        if (c == '/') {
            uint32_t length = i - start;
            if (!length || (length == 1u && name.data[start] == '.') ||
                (length == 2u && name.data[start] == '.' && name.data[start + 1u] == '.')) return false;
            start = i + 1u;
        } else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) {
            return false;
        }
    }
    return true;
}

/* Bound the scan and exclude terminal control characters in diagnostics. */
static bool valid_type_name(const char *name)
{
    uint32_t i;
    if (!name) return false;
    for (i = 0; i <= OBJECT_TYPE_NAME_MAX; ++i) {
        uint8_t c = (uint8_t)name[i];
        if (!c) return i != 0;
        if (i == OBJECT_TYPE_NAME_MAX || c < 0x20u || c > 0x7Eu) return false;
    }
    return false;
}

ObjectResult object_create(const ObjectType *type, Object **out_object)
{
    Object *object;
    uint32_t size;
    if (!manager_initialized) return OBJECT_NOT_READY;
    if (!type || !out_object || !type->body_size || !valid_type_name(type->name)) return OBJECT_INVALID;
    if (type->body_size > OBJECT_MAX_U32 - (uint32_t)sizeof(Object)) return OBJECT_NO_MEMORY;
    size = (uint32_t)sizeof(Object) + type->body_size;
    object = kmalloc(size);
    if (!object) return OBJECT_NO_MEMORY;
    kmemset(object, 0, size);
    object->type = type;
    object->references = 1;
    *out_object = object;
    return OBJECT_OK;
}

static ObjectResult retain_reference(Object *object)
{
    if (object->references == OBJECT_MAX_U32) return OBJECT_REFERENCE_LIMIT;
    ++object->references;
    return OBJECT_OK;
}

ObjectResult object_retain(Object *object)
{
    if (!manager_initialized) return OBJECT_NOT_READY;
    if (!object || !object->references || object->references <= object->publications) return OBJECT_INVALID;
    return retain_reference(object);
}

const ObjectType *object_type(const Object *object)
{
    if (!manager_initialized || !object || !object->references) return 0;
    return object->type;
}

void *object_body(Object *object, const ObjectType *expected_type)
{
    if (!manager_initialized || !object || !object->references || !expected_type || object->type != expected_type) return 0;
    return object + 1;
}

/* No registry data is touched after calling a destructor. A callback can
   release other objects; it must never retain or release the dying object. */
static void drop_reference(Object *object)
{
    if (--object->references == 0) {
        const ObjectType *type = object->type;
        if (object->publications) kernel_panic("Object", "published object reached zero references");
        if (type->destroy) type->destroy(object + 1);
        kfree(object);
    }
}

void object_release(Object *object)
{
    if (!object) return;
    /* Published references belong to the registry, never to callers. */
    if (!manager_initialized || !object->references || object->references <= object->publications)
        kernel_panic("Object", "release without caller reference");
    drop_reference(object);
}

ObjectResult object_publish(StringView name, Object *object)
{
    DictionaryResult result;
    Dictionary *target;
    bool created = false;
    if (!manager_initialized) return OBJECT_NOT_READY;
    if (!valid_name(name) || !object || !object->references || object->references <= object->publications)
        return OBJECT_INVALID;
    if (registry && dictionary_contains(registry, name)) return OBJECT_NAME_EXISTS;
    if (object->references == OBJECT_MAX_U32 || object->publications == OBJECT_MAX_U32) return OBJECT_REFERENCE_LIMIT;
    target = registry;
    if (!target) {
        target = dictionary_create();
        if (!target) return OBJECT_NO_MEMORY;
        created = true;
    }
    result = dictionary_insert(target, name, object);
    if (result != DICTIONARY_OK) {
        if (created) dictionary_destroy(target);
        return result == DICTIONARY_EXISTS ? OBJECT_NAME_EXISTS : OBJECT_NO_MEMORY;
    }
    /* No allocation or callback remains after insertion, so these changes form
       one publication in the current single-CPU non-preemptive context. */
    ++object->references;
    ++object->publications;
    registry = target;
    return OBJECT_OK;
}

ObjectResult object_lookup(StringView name, const ObjectType *expected_type, Object **out_object)
{
    Object *object;
    ObjectResult result;
    void *value;
    if (!manager_initialized) return OBJECT_NOT_READY;
    if (!valid_name(name) || !expected_type || !out_object) return OBJECT_INVALID;
    if (!registry || !dictionary_get(registry, name, &value)) return OBJECT_NOT_FOUND;
    object = value;
    if (object->type != expected_type) return OBJECT_TYPE_MISMATCH;
    /* The registry holds the reference that protects this lookup. */
    result = retain_reference(object);
    if (result != OBJECT_OK) return result;
    *out_object = object;
    return OBJECT_OK;
}

ObjectResult object_unpublish(StringView name, Object *expected_object)
{
    void *value;
    if (!manager_initialized) return OBJECT_NOT_READY;
    if (!valid_name(name) || !expected_object || !expected_object->references ||
        expected_object->references <= expected_object->publications) return OBJECT_INVALID;
    if (!registry || !dictionary_get(registry, name, &value)) return OBJECT_NOT_FOUND;
    if (value != expected_object) return OBJECT_WRONG_OBJECT;
    if (!dictionary_remove(registry, name, 0) || !expected_object->publications)
        kernel_panic("Object", "registry ownership corrupted");
    --expected_object->publications;
    /* An empty dictionary otherwise keeps heap pages alive forever. Remove it
       before dropping the reference, so callbacks may publish safely. */
    if (!dictionary_count(registry)) {
        dictionary_destroy(registry);
        registry = 0;
    }
    drop_reference(expected_object);
    return OBJECT_OK;
}
