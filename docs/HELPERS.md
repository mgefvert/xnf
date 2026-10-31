# Strings and containers

[inc/helpers.h](../inc/helpers.h) declares the freestanding helpers implemented in
[src/helpers.c](../src/helpers.c). They need no hosted C library. Allocating
operations use `kmalloc` and `kfree`; the heap must already be initialized.
They inherit its one-CPU, non-preemptive normal-context restriction: do not
call them from IRQ or exception handlers. Pure string and memory operations do
not allocate, but callers must still synchronize access to shared data.

Array lists and dictionaries own their metadata but **borrow all value
pointers**. Values may be null and are never freed or dereferenced by those
containers. An output pointer on a lookup/removal may be null when the value is
not needed. A Boolean result distinguishes an absent entry from a stored null;
on failure output arguments are unchanged. The linked list instead links
caller-owned objects directly, as described below. Except where explicitly
allowed below, object, node, buffer, and string pointers must be valid.
Destroying a null container or string is a no-op; other operations require a
live object.

Allocation failures and integer overflow leave the existing object and its
contents unchanged. `kmalloc` returns null before heap initialization or under
memory pressure, so creation and growth can fail; check their return values.

## Byte and string primitives

`StringView` is a borrowed `{data, length}` pair: its length counts bytes and
it does not require a terminator. A null data pointer is valid only with length
zero. `string_view(data, length)` makes a view over an existing readable byte
range; `string_view_cstr(s)` measures an existing terminated C string.
`STRING_VIEW_LITERAL("text")` constructs a view without a runtime scan; pass
only a string literal or character array, not a pointer. The caller guarantees
the bytes remain readable while the view is used. Views can contain zero bytes.
`string_view_equal` compares length then contents; invalid views compare false.

`kstrlen`, `kstrnlen`, `kstrcmp`, `kstrncmp`, `kstrchr`, and `kstrrchr` follow
the usual byte-oriented C string semantics. Unbounded operations require valid
terminated strings; bounded operations require readable bytes through a
terminator or their limit. `kmemcmp` and `kmemset` operate on valid byte ranges.
For zero-length byte operations, null pointers are permitted. These low-level
functions cannot discover whether an arbitrary pointer really has enough space.

`kmemcpy_checked(dst, capacity, src, size)` and `kmemmove_checked(...)` reject
an insufficient destination capacity or null nonempty input without writing.
The caller must supply truthful capacities and a readable source of `size`
bytes. Copy requires nonoverlapping ranges; move handles overlap. `kstr_copy`
and `kstr_append` accept a source view and destination capacity, support
overlap, and write a trailing zero on success. Append requires the destination
to contain a terminator within its capacity. Both reject insufficient space or
invalid arguments without changing the destination. A zero capacity fails.

## Managed strings

`KString` owns one heap allocation containing length, capacity, characters,
and a trailing zero. `kstring_create(view)` copies its input; empty views are
accepted. `kstring_destroy` frees the allocation. `kstring_length` and
`kstring_capacity` are constant-time byte counts; capacity excludes the trailing
zero. `kstring_data` returns a read-only terminated pointer and `kstring_view`
returns a borrowed view. Zero bytes inside a view are retained; C-string
consumers such as `kputs` stop at the first zero.

`kstring_reserve(&s, capacity)`, `kstring_assign(&s, view)`,
`kstring_append(&s, view)`, and `kstring_insert(&s, index, view)` return false
on invalid arguments, overflow, or allocation failure. Insert accepts positions
from zero through the current length. Growth doubles capacity, starting at
eight bytes where applicable, and allocates/copies/frees because the heap has
no realloc. Mutators may move the handle; use the updated `s`, and reacquire
any borrowed data pointer or view after a successful mutation. Input views may
refer to bytes within the same string's current contents, including during
growth or insertion. Insertion of an aliased view may allocate even if spare
capacity exists. On failure the handle and contents are unchanged.

## Array List

`list_create`/`list_destroy` manage the list and its backing pointer array.
`list_count` and `list_capacity` report entries and reserved slots. Append and
insert return false on allocation failure; insertion accepts indices through
count and shifts later pointers. Capacity starts at eight slots and doubles;
`list_reserve` guarantees at least the requested capacity. It checks pointer
array multiplication and growth for overflow. `list_get`, `list_set`, and
`list_remove` return false for an out-of-range index; set optionally reports
the previous pointer, and remove shifts later pointers. `list_pop` removes the
last pointer. `list_clear` keeps capacity; `list_shrink_to_fit` explicitly
shrinks to count, releasing the buffer entirely when empty. Shrinking may fail
without changing the list. Array growth or shrinking does not move the objects
pointed to by its entries. Access by index is constant time, append is amortized
constant time, and middle insertion/removal is linear.

## Doubly linked list

Each caller-owned object starts with a `LinkedListNode` member. Its first
pointer is `previous` and its second is `next`; both point to the embedded
node at the beginning of another object, or are null at an end. Declare the
member using the actual `LinkedListNode` type so C accesses have the correct
type, and initialize both links to null before insertion. A node pointer can
be converted back to its enclosing object pointer because the node is the
first member. The object may have other fields after the links.

`linked_list_create` allocates only the list header. The caller owns every
object and must keep it alive and unmoved while linked. Insertion, traversal,
removal, and clearing allocate and free nothing. `linked_list_destroy` clears
the list and frees the header, but never frees objects. Removal, pop, clear,
and destroy reset the removed nodes' links to null, leaving their objects
valid and ready for reuse. `linked_list_count` reports the number of nodes.

`linked_list_push_front`, `linked_list_push_back`, `linked_list_insert_before`,
and `linked_list_insert_after` accept an embedded node and return false for a
null node, a null anchor, a visibly linked node, or count overflow. The caller
must ensure each inserted node is detached and belongs to no other list, and
each insertion anchor or removed node belongs to the specified list. These
preconditions cannot be fully checked from two links: in particular, a node
alone in another list also has two null links. Violating them can corrupt lists.
Inserting or removing a known node is constant time; clear and destroy visit
each node. End and traversal accessors return node pointers or null at the end.
`linked_list_remove` returns whether it removed a node;
`linked_list_pop_front` and `linked_list_pop_back` return the removed node or
null for an empty list. There is no separate value pointer or indexed access.
Callers must synchronize concurrent access, and must not modify links directly
while an object is in a list.

```c
typedef struct WorkItem {
    LinkedListNode links; /* Must be the first member. */
    uint32_t id;
} WorkItem;

WorkItem item = {{0, 0}, 42};
LinkedList *work = linked_list_create();
if (work && linked_list_push_back(work, &item.links)) {
    WorkItem *first = (WorkItem *)linked_list_first(work);
    /* first is &item; the list does not own it. */
    (void)first;
}
linked_list_destroy(work); /* Detaches item; its lifetime remains with the caller. */
```

## Dictionary

The dictionary is a separate-chaining hash table. Keys are case-sensitive
`StringView`s; FNV-1a hashes their bytes and comparison checks length and bytes,
so hash collisions are safe. Each entry stores its own copy of the key bytes
and a trailing zero in the same heap allocation as its metadata. Embedded zero
bytes remain part of the key. `dictionary_create`, `dictionary_destroy`, and
`dictionary_clear` manage the table and entries; clear keeps its bucket array.
`dictionary_count` reports entries. `dictionary_reserve(d, n)` ensures
room for `n` entries at a maximum 75% load, growing power-of-two bucket arrays;
failure leaves the table unchanged.

`dictionary_insert` reports `DICTIONARY_OK`, `DICTIONARY_EXISTS`,
`DICTIONARY_NO_MEMORY`, or `DICTIONARY_INVALID` for an invalid dictionary or
view. `dictionary_set` inserts or replaces a value and can report whether an
entry existed and its previous value; on replacement it retains the existing
owned key. `dictionary_get` and `dictionary_contains` check membership;
`dictionary_remove` deletes an entry and optionally returns its value.
`dictionary_iter_begin` and `dictionary_iter_next` enumerate borrowed key
views and values in unspecified order. Structural mutations (including a
successful reserve that grows the table) invalidate iterators. A key view
returned by iteration borrows the entry's own bytes: it stays valid until that
entry is removed, the dictionary is cleared, or it is destroyed. Rehashing
and replacing a value do not move or change the stored key. Replacing a value
does not change table structure, but avoid changing values while using an
iterator if a consistent snapshot is required.

**The dictionary owns its key copies.** Input views need to remain readable and
unchanged only for the duration of the call. The original string can be changed
or freed immediately after a successful insertion. The dictionary releases each
key with its entry on removal, clear, or destruction. It still borrows values.
An entry size that would overflow, failure to allocate the entry, or failure to
grow the table returns `DICTIONARY_NO_MEMORY` without changing the dictionary
or outputs. The empty view is a valid key.

For example, the dictionary keeps the key after the source string is destroyed:

```c
KString *name = kstring_create(STRING_VIEW_LITERAL("console"));
Dictionary *devices = dictionary_create();
if (name && devices &&
    dictionary_insert(devices, kstring_view(name), device) == DICTIONARY_OK) {
    kstring_destroy(name);
    name = 0;
    void *found;
    if (dictionary_get(devices, STRING_VIEW_LITERAL("console"), &found)) {
        /* found is the caller-owned device pointer. */
    }
}
dictionary_destroy(devices);
kstring_destroy(name);
```

## Validation

The optional `tests/kernel/helpers_test.c` is linked only in a test build.
`python tests/helpers_qemu.py --build cmake-build-tests` runs its entry in
disposable 128/512 MiB guests and checks ownership restoration, mappings,
timer progress, and fault/reset logs. The guest covers string bounds and
aliasing, list growth, caller-owned linked objects and heap-free linking,
dictionary collisions, owned-key
lifetimes and rehashing, and rollback under real heap exhaustion. See
[BUILD.md](BUILD.md) for configuration and tool discovery.
