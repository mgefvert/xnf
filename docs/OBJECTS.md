# Kernel object manager

[inc/object.h](../inc/object.h) and [src/object.c](../src/object.c) provide typed,
heap-owned objects and a registry of named objects. They provide lifetime and
discovery; individual subsystems supply operations and state. There are no
device drivers, scheduler, waiting primitives, user handles, or access control
in this interface. Names are kernel registry keys, not filesystem paths.

## Initialization and context

`kernel_main` calls `object_manager_init()` immediately after `kheap_init()`.
Initialization allocates nothing, so empty boot and synthetic PMM fixtures do
not acquire heap pages. Calling it before heap initialization returns
`OBJECT_NOT_READY`; calling it again returns `OBJECT_ALREADY_INITIALIZED`.
Create, retain, publish, lookup, and unpublish return `OBJECT_NOT_READY` before
manager initialization. `object_body` and `object_type` return null, and `object_release(NULL)`
is a no-op, including before initialization.

All object-manager operations, including callbacks and release, are restricted
to one CPU and non-preemptive normal context. Do not call them from IRQ or
exception handlers. They inherit the heap and Dictionary contracts and preserve
the caller's interrupt-enable flag; they do not synchronize access. Competing
scheduled threads, SMP, or interrupt-side access require a new synchronization design. In
particular, lookup plus reference acquisition and name removal plus reference
release must become indivisible. Destructors must run outside future locks.

## Types and allocation

An `ObjectType` specifies a human-readable `name`, the nonzero byte size of a
subsystem-defined body, and an optional `destroy(void *body)` callback. The
name is a nonempty, zero-terminated string of at most `OBJECT_TYPE_NAME_MAX`
(63) printable ASCII bytes, including spaces but excluding control characters.
This permits safe display on XNF's current text console. The descriptor and
the bytes of its name are borrowed, immutable, and must remain alive until
every object of that type is destroyed; normally both are static data in the
kernel image. Its callback code must also remain available throughout that time.

Type names are labels for people, need not be unique, and are not registry
keys. Type identity is the descriptor pointer, not its name or a structural
comparison. `object_type(object)` returns the borrowed descriptor and its
name is available as `object_type(object)->name`. The caller must hold an
object reference while using the returned pointer. It returns null for a null
object or before manager initialization. The registry does not yet expose
enumeration; a future registry walker can use the same descriptor and name.

`object_create(type, &object)` returns `OBJECT_OK` and one caller-owned
reference. The manager allocates a private header and the body together with
`kmalloc`; the body is zeroed and 16-byte aligned. `object_body(object, type)`
returns a borrowed pointer to that body when the descriptor matches, or null
on a mismatch. The caller must own a reference throughout its use. Never pass
the body or the manager's private header to `kfree`; the manager frees the
allocation when the final reference is released.

Create rejects null arguments, invalid type names, and zero-sized bodies with `OBJECT_INVALID`,
and reports size overflow or allocation failure as `OBJECT_NO_MEMORY`. On
failure its output is unchanged. Object references and body pointers must be
valid and live; the manager cannot safely validate arbitrary or stale pointers.

Initialize all body fields before publishing. If initialization fails, the
creator releases its reference. The destructor must tolerate a zeroed or
partially initialized body, free any resources it owns, and return without
freeing the body itself. The manager sets the reference count to zero before
calling it and frees the object afterward. A destructor must not retain,
publish, or release its own dying object; it may manage other live objects in
normal context. Cyclic strong references must be broken explicitly by the
subsystems that establish them.

## References and names

Each successful creation, lookup, or retain gives the caller one reference;
each must be matched by `object_release`. `object_retain` requires an existing
caller-owned reference and returns `OBJECT_REFERENCE_LIMIT` instead of wrapping
the counter. Release of a live object without a caller-owned reference panics
when the manager can detect it. As with any reference-counted API, it cannot
identify which caller owns a particular reference or detect all stale pointers.

`object_publish(name, object)` requires a caller-owned reference. It copies
the name and gives the registry one additional strong reference. It returns
`OBJECT_NAME_EXISTS` for a duplicate without replacing the entry. One object
may be published under multiple distinct names, each with its own reference.

`object_lookup(name, expected_type, &object)` checks the type descriptor and
returns a new caller-owned reference. Missing names return `OBJECT_NOT_FOUND`,
and type mismatches return `OBJECT_TYPE_MISMATCH`. A successful lookup remains
valid after unpublishing as long as the caller retains its reference.

`object_unpublish(name, expected_object)` requires a caller-owned reference
to the expected object. It returns `OBJECT_WRONG_OBJECT` if the name now
identifies another object. On success it removes the name and drops exactly
the registry's reference. When the last name is removed, the empty Dictionary
is freed too. The last reference triggers the destructor exactly once. The
manager does not mark an object offline: a device subsystem must define and
enforce removal/operation semantics separately.

All failed operations leave output pointers and existing ownership unchanged.
Publication or lookup returns `OBJECT_REFERENCE_LIMIT` on reference overflow;
publication returns `OBJECT_NO_MEMORY` if it cannot allocate its registry or
entry. The registry owns copied keys, while its Dictionary stores object
pointers; the manager explicitly accounts for the corresponding strong
references. It does not expose enumeration or a list of unnamed objects.

## Name syntax

Names are case-sensitive `StringView`s, from 2 through `OBJECT_NAME_MAX` (127)
bytes, beginning with `/`. Components are nonempty and separated by single
slashes; they may contain ASCII letters, digits, `_`, `-`, and `.`. Components
`.` and `..`, a trailing slash, embedded zeroes, and other bytes are rejected
with `OBJECT_INVALID`. For example, `/device/console0` is valid, while `/`,
`/device//console0`, and `/device/../console0` are not. The slash is part of
a flat key: parent directories and path traversal do not exist. Input name
bytes must remain readable for the duration of the call; publication copies
them and later changes to the source do not change the registered name.

## Example

The following illustrates ownership for a future subsystem; it does not
install a console driver:

```c
typedef struct ExampleDevice { void *resource; } ExampleDevice;

static void example_destroy(void *body)
{
    ExampleDevice *device = body;
    kfree(device->resource); /* Also valid if initialization left it NULL. */
}

static const ObjectType example_type = {"Example device", sizeof(ExampleDevice), example_destroy};

Object *device;
if (object_create(&example_type, &device) == OBJECT_OK) {
    ExampleDevice *body = object_body(device, &example_type);
    /* Initialize body and any owned resources before publication. */
    (void)body;
    if (object_publish(STRING_VIEW_LITERAL("/device/console0"), device) == OBJECT_OK) {
        /* The registry now owns its own reference. */
    }
    object_release(device); /* Release the creator's reference. */
}
```

To unpublish later, acquire a reference with `object_lookup`, call
`object_unpublish` with that exact object, then release the lookup reference.

## Validation

`tests/kernel/object_test.c` is built only with `XNF_BUILD_TESTS=ON`.
`python tests/object_qemu.py --build cmake-build-tests` runs disposable guests
at 128/512 MiB, checks pre-heap initialization in a separate guest, and tests
an invalid release in a panic guest. Positive tests cover owned names, type
checks, type-name validation and identity, aliases, name reuse, references after unpublication, partial-body
destruction, real heap exhaustion and recovery, interrupt state, and complete
heap/PMM/VMM ownership cleanup. A test-only fixture reaches reference-count
saturation without billions of operations. The host also checks page tables, control
registers, timer progress, and fault/reset logs. See [BUILD.md](BUILD.md).
