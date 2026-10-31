/*
 * Typed, heap-owned kernel objects and a named registry of strong references.
 * See docs/OBJECTS.md for initialization, lifetime, and execution contracts.
 */
#ifndef XNF_OBJECT_H
#define XNF_OBJECT_H

#include "helpers.h"

#define OBJECT_NAME_MAX 127u
#define OBJECT_TYPE_NAME_MAX 63u

typedef struct Object Object;

/* The descriptor and its terminated name are immutable and must outlive every
   object of that type. Names are 1..OBJECT_TYPE_NAME_MAX printable ASCII bytes;
   identity is the descriptor address. The manager zeroes body_size bytes;
   destroy may be NULL and must tolerate a partially initialized body. */
typedef struct ObjectType {
    const char *name;
    uint32_t body_size;
    void (*destroy)(void *body);
} ObjectType;

typedef enum ObjectResult {
    OBJECT_OK,
    OBJECT_INVALID,
    OBJECT_NOT_READY,
    OBJECT_ALREADY_INITIALIZED,
    OBJECT_NO_MEMORY,
    OBJECT_NAME_EXISTS,
    OBJECT_NOT_FOUND,
    OBJECT_TYPE_MISMATCH,
    OBJECT_WRONG_OBJECT,
    OBJECT_REFERENCE_LIMIT
} ObjectResult;

/* Initialize once after kheap_init; allocates nothing. Normal context only.
   Manager operations exclude preemption; callers synchronize object bodies. */
ObjectResult object_manager_init(void);

/* On success create and lookup return one owned reference in out_object.
   Failed calls leave outputs and existing objects unchanged. */
ObjectResult object_create(const ObjectType *type, Object **out_object);
ObjectResult object_lookup(StringView name, const ObjectType *expected_type, Object **out_object);
ObjectResult object_retain(Object *object);
/* Release exactly one caller-owned reference. NULL is a no-op. */
void object_release(Object *object);

/* Return the borrowed immutable descriptor, or NULL for NULL. The caller must
   keep its object reference while using the descriptor and its name. */
const ObjectType *object_type(const Object *object);

/* A body pointer is borrowed while the caller holds a reference. Returns NULL
   for a wrong type. Its storage must never be freed directly. */
void *object_body(Object *object, const ObjectType *expected_type);

/* Publish copies a name and acquires a registry reference. Unpublish requires
   a caller-owned reference to the expected object and releases the registry's
   reference. Multiple distinct names for one object are permitted. */
ObjectResult object_publish(StringView name, Object *object);
ObjectResult object_unpublish(StringView name, Object *expected_object);

#endif
