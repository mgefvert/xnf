/*
 * Heap-backed strings and pointer containers for normal kernel context.
 * Containers borrow values; dictionary entries own key bytes. See KERNEL_HELPERS.md.
 */
#include "helpers.h"
#include "kheap.h"

#define MAX_U32 (~(uint32_t)0)

StringView string_view(const char *data, uint32_t length)
{
    StringView view = {data, length};
    return view;
}

uint32_t kstrnlen(const char *string, uint32_t limit)
{
    uint32_t n = 0;
    while (n < limit && string[n] != 0) ++n;
    return n;
}

uint32_t kstrlen(const char *string)
{
    uint32_t n = 0;
    while (string[n] != 0) ++n;
    return n;
}

StringView string_view_cstr(const char *string)
{
    return string_view(string, kstrlen(string));
}

int kmemcmp(const void *left, const void *right, uint32_t size)
{
    const uint8_t *a = left, *b = right;
    uint32_t i;
    for (i = 0; i < size; ++i) {
        if (a[i] != b[i]) return (int)a[i] - (int)b[i];
    }
    return 0;
}

int kstrncmp(const char *left, const char *right, uint32_t limit)
{
    uint32_t i;
    for (i = 0; i < limit; ++i) {
        uint8_t a = (uint8_t)left[i], b = (uint8_t)right[i];
        if (a != b) return (int)a - (int)b;
        if (a == 0) return 0;
    }
    return 0;
}

int kstrcmp(const char *left, const char *right)
{
    while (*left && (uint8_t)*left == (uint8_t)*right) { ++left; ++right; }
    return (int)(uint8_t)*left - (int)(uint8_t)*right;
}

const char *kstrchr(const char *string, int character)
{
    uint8_t c = (uint8_t)character;
    do { if ((uint8_t)*string == c) return string; } while (*string++ != 0);
    return 0;
}

const char *kstrrchr(const char *string, int character)
{
    const char *result = 0;
    uint8_t c = (uint8_t)character;
    do { if ((uint8_t)*string == c) result = string; } while (*string++ != 0);
    return result;
}

void *kmemset(void *destination, int value, uint32_t size)
{
    uint8_t *p = destination;
    uint32_t i;
    for (i = 0; i < size; ++i) p[i] = (uint8_t)value;
    return destination;
}

bool kmemcpy_checked(void *destination, uint32_t capacity, const void *source, uint32_t size)
{
    uint8_t *d = destination;
    const uint8_t *s = source;
    uint32_t i;
    if (size > capacity || (size && (!d || !s))) return false;
    for (i = 0; i < size; ++i) d[i] = s[i];
    return true;
}

bool kmemmove_checked(void *destination, uint32_t capacity, const void *source, uint32_t size)
{
    uint8_t *d = destination;
    const uint8_t *s = source;
    uint32_t i;
    if (size > capacity || (size && (!d || !s))) return false;
    /* Compare integer addresses: relational comparison of unrelated C pointers
       is undefined. Reverse only when destination starts inside the source. */
    if ((uint32_t)d > (uint32_t)s && (uint32_t)d - (uint32_t)s < size) {
        for (i = size; i != 0; --i) d[i - 1u] = s[i - 1u];
    } else {
        for (i = 0; i < size; ++i) d[i] = s[i];
    }
    return true;
}

static bool view_valid(StringView view)
{
    return view.data != 0 || view.length == 0;
}

bool string_view_equal(StringView left, StringView right)
{
    return view_valid(left) && view_valid(right) && left.length == right.length &&
           kmemcmp(left.data, right.data, left.length) == 0;
}

bool kstr_copy(char *destination, uint32_t capacity, StringView source)
{
    if (!destination || !view_valid(source) || !capacity || source.length >= capacity) return false;
    kmemmove_checked(destination, capacity, source.data, source.length);
    destination[source.length] = 0;
    return true;
}

bool kstr_append(char *destination, uint32_t capacity, StringView source)
{
    uint32_t length;
    if (!destination || !view_valid(source) || !capacity) return false;
    length = kstrnlen(destination, capacity);
    if (length == capacity || source.length >= capacity - length) return false;
    kmemmove_checked(destination + length, capacity - length, source.data, source.length);
    destination[length + source.length] = 0;
    return true;
}

struct KString { uint32_t length, capacity; char data[]; };

static uint32_t string_max_capacity(void)
{
    return MAX_U32 - (uint32_t)sizeof(KString) - 1u;
}

static KString *string_allocate(uint32_t capacity)
{
    KString *result;
    if (capacity > string_max_capacity()) return 0;
    result = kmalloc((uint32_t)sizeof(KString) + capacity + 1u);
    if (result) { result->length = 0; result->capacity = capacity; result->data[0] = 0; }
    return result;
}

/* Geometric growth is clamped before arithmetic can wrap. */
static bool string_growth(uint32_t old, uint32_t needed, uint32_t *capacity)
{
    uint32_t limit = string_max_capacity(), n = old ? old : 8u;
    if (needed > limit) return false;
    while (n < needed) {
        if (n > limit / 2u) { n = limit; break; }
        n *= 2u;
    }
    *capacity = n;
    return true;
}

KString *kstring_create(StringView initial)
{
    KString *result;
    if (!view_valid(initial)) return 0;
    result = string_allocate(initial.length);
    if (!result) return 0;
    kmemcpy_checked(result->data, initial.length, initial.data, initial.length);
    result->length = initial.length;
    result->data[initial.length] = 0;
    return result;
}

void kstring_destroy(KString *string) { kfree(string); }
uint32_t kstring_length(const KString *string) { return string->length; }
uint32_t kstring_capacity(const KString *string) { return string->capacity; }
const char *kstring_data(const KString *string) { return string->data; }
StringView kstring_view(const KString *string) { return string_view(string->data, string->length); }

bool kstring_reserve(KString **string, uint32_t capacity)
{
    KString *old, *next;
    uint32_t grown;
    if (!string || !*string) return false;
    old = *string;
    if (capacity <= old->capacity) return true;
    if (!string_growth(old->capacity, capacity, &grown) || !(next = string_allocate(grown))) return false;
    kmemcpy_checked(next->data, grown, old->data, old->length);
    next->length = old->length;
    next->data[next->length] = 0;
    *string = next;
    kfree(old);
    return true;
}

/* This shared splice keeps input alive until any replacement is complete.
   When inserting an aliased view, use a fresh allocation even if capacity is
   sufficient: shifting in place could overwrite bytes still needed as input. */
static bool string_splice(KString **string, uint32_t index, uint32_t removed, StringView value)
{
    KString *old, *next;
    uint32_t length, capacity, tail;
    bool aliases;
    if (!string || !*string || !view_valid(value)) return false;
    old = *string;
    if (index > old->length || removed > old->length - index ||
        value.length > string_max_capacity() - (old->length - removed)) return false;
    length = old->length - removed + value.length;
    tail = old->length - index - removed;
    aliases = value.length && (uint32_t)value.data >= (uint32_t)old->data &&
              (uint32_t)value.data - (uint32_t)old->data <= old->length;
    if (length > old->capacity || (aliases && tail && value.length != removed)) {
        if (!string_growth(old->capacity, length, &capacity) || !(next = string_allocate(capacity))) return false;
        kmemcpy_checked(next->data, capacity, old->data, index);
        kmemcpy_checked(next->data + index, capacity - index, value.data, value.length);
        kmemcpy_checked(next->data + index + value.length, capacity - index - value.length,
                        old->data + index + removed, tail);
        next->length = length;
        next->data[length] = 0;
        *string = next;
        kfree(old);
    } else {
        kmemmove_checked(old->data + index + value.length, old->capacity - index - value.length,
                         old->data + index + removed, tail);
        kmemmove_checked(old->data + index, old->capacity - index, value.data, value.length);
        old->length = length;
        old->data[length] = 0;
    }
    return true;
}

bool kstring_assign(KString **string, StringView value)
{
    return string && *string && string_splice(string, 0, (*string)->length, value);
}
bool kstring_append(KString **string, StringView value)
{
    return string && *string && string_splice(string, (*string)->length, 0, value);
}
bool kstring_insert(KString **string, uint32_t index, StringView value)
{
    return string_splice(string, index, 0, value);
}

struct List { void **items; uint32_t count, capacity; };
List *list_create(void)
{
    List *list = kmalloc(sizeof(*list));
    if (list) { list->items = 0; list->count = 0; list->capacity = 0; }
    return list;
}
void list_destroy(List *list) { if (list) { kfree(list->items); kfree(list); } }
void list_clear(List *list) { list->count = 0; }
uint32_t list_count(const List *list) { return list->count; }
uint32_t list_capacity(const List *list) { return list->capacity; }

static bool list_resize(List *list, uint32_t capacity)
{
    void **items;
    if (capacity > MAX_U32 / sizeof(*items)) return false;
    if (!capacity) { kfree(list->items); list->items = 0; list->capacity = 0; return true; }
    items = kmalloc(capacity * sizeof(*items));
    if (!items) return false;
    kmemcpy_checked(items, capacity * sizeof(*items), list->items, list->count * sizeof(*items));
    kfree(list->items);
    list->items = items;
    list->capacity = capacity;
    return true;
}

bool list_reserve(List *list, uint32_t capacity)
{
    uint32_t n = list->capacity ? list->capacity : 8u;
    uint32_t limit = MAX_U32 / sizeof(*list->items);
    if (capacity <= list->capacity) return true;
    if (capacity > limit) return false;
    while (n < capacity) {
        if (n > limit / 2u) { n = limit; break; }
        n *= 2u;
    }
    return list_resize(list, n);
}
bool list_shrink_to_fit(List *list)
{
    return list->count == list->capacity || list_resize(list, list->count);
}
bool list_insert(List *list, uint32_t index, void *value)
{
    if (index > list->count || list->count == MAX_U32 || !list_reserve(list, list->count + 1u)) return false;
    kmemmove_checked(list->items + index + 1u, (list->capacity - index - 1u) * sizeof(void *),
                     list->items + index, (list->count - index) * sizeof(void *));
    list->items[index] = value;
    ++list->count;
    return true;
}
bool list_append(List *list, void *value) { return list_insert(list, list->count, value); }
bool list_get(const List *list, uint32_t index, void **out_value)
{
    if (index >= list->count) return false;
    if (out_value) *out_value = list->items[index];
    return true;
}
bool list_set(List *list, uint32_t index, void *value, void **out_previous)
{
    if (index >= list->count) return false;
    if (out_previous) *out_previous = list->items[index];
    list->items[index] = value;
    return true;
}
bool list_remove(List *list, uint32_t index, void **out_value)
{
    if (index >= list->count) return false;
    if (out_value) *out_value = list->items[index];
    --list->count;
    kmemmove_checked(list->items + index, (list->capacity - index) * sizeof(void *),
                     list->items + index + 1u, (list->count - index) * sizeof(void *));
    return true;
}
bool list_pop(List *list, void **out_value)
{
    return list->count && list_remove(list, list->count - 1u, out_value);
}

struct LinkedListNode {
    struct LinkedListNode *previous, *next;
    LinkedList *owner;
    void *value;
};
struct LinkedList { LinkedListNode *first, *last; uint32_t count; };

LinkedList *linked_list_create(void)
{
    LinkedList *list = kmalloc(sizeof(*list));
    if (list) { list->first = 0; list->last = 0; list->count = 0; }
    return list;
}
void linked_list_clear(LinkedList *list)
{
    LinkedListNode *node = list->first;
    while (node) { LinkedListNode *next = node->next; kfree(node); node = next; }
    list->first = list->last = 0;
    list->count = 0;
}
void linked_list_destroy(LinkedList *list) { if (list) { linked_list_clear(list); kfree(list); } }
uint32_t linked_list_count(const LinkedList *list) { return list->count; }
LinkedListNode *linked_list_first(const LinkedList *list) { return list->first; }
LinkedListNode *linked_list_last(const LinkedList *list) { return list->last; }
LinkedListNode *linked_list_next(const LinkedListNode *node) { return node->next; }
LinkedListNode *linked_list_previous(const LinkedListNode *node) { return node->previous; }
void *linked_list_value(const LinkedListNode *node) { return node->value; }

static LinkedListNode *linked_insert(LinkedList *list, LinkedListNode *previous,
                                     LinkedListNode *next, void *value)
{
    LinkedListNode *node;
    if (list->count == MAX_U32 || !(node = kmalloc(sizeof(*node)))) return 0;
    node->previous = previous; node->next = next; node->owner = list; node->value = value;
    if (previous) previous->next = node; else list->first = node;
    if (next) next->previous = node; else list->last = node;
    ++list->count;
    return node;
}
LinkedListNode *linked_list_push_front(LinkedList *list, void *value)
{ return linked_insert(list, 0, list->first, value); }
LinkedListNode *linked_list_push_back(LinkedList *list, void *value)
{ return linked_insert(list, list->last, 0, value); }
LinkedListNode *linked_list_insert_before(LinkedList *list, LinkedListNode *node, void *value)
{ return node && node->owner == list ? linked_insert(list, node->previous, node, value) : 0; }
LinkedListNode *linked_list_insert_after(LinkedList *list, LinkedListNode *node, void *value)
{ return node && node->owner == list ? linked_insert(list, node, node->next, value) : 0; }
bool linked_list_remove(LinkedList *list, LinkedListNode *node, void **out_value)
{
    if (!node || node->owner != list) return false;
    if (out_value) *out_value = node->value;
    if (node->previous) node->previous->next = node->next; else list->first = node->next;
    if (node->next) node->next->previous = node->previous; else list->last = node->previous;
    --list->count;
    kfree(node);
    return true;
}
bool linked_list_pop_front(LinkedList *list, void **out_value)
{ return linked_list_remove(list, list->first, out_value); }
bool linked_list_pop_back(LinkedList *list, void **out_value)
{ return linked_list_remove(list, list->last, out_value); }

typedef struct DictionaryEntry {
    struct DictionaryEntry *next;
    StringView key;
    void *value;
    uint32_t hash;
    /* One allocation owns both this metadata and a terminated key copy. */
    char key_bytes[];
} DictionaryEntry;
struct Dictionary { DictionaryEntry **buckets; uint32_t capacity, count; };

/* FNV-1a hashes bytes with unsigned 32-bit wraparound. Equality still checks
   length and bytes, so collisions do not make distinct keys interchangeable. */
static uint32_t dictionary_hash(StringView key)
{
    uint32_t h = 2166136261u, i;
    for (i = 0; i < key.length; ++i) h = (h ^ (uint8_t)key.data[i]) * 16777619u;
    return h;
}

Dictionary *dictionary_create(void)
{
    Dictionary *dictionary = kmalloc(sizeof(*dictionary));
    if (dictionary) { dictionary->buckets = 0; dictionary->capacity = dictionary->count = 0; }
    return dictionary;
}
void dictionary_clear(Dictionary *dictionary)
{
    uint32_t i;
    for (i = 0; i < dictionary->capacity; ++i) {
        DictionaryEntry *entry = dictionary->buckets[i];
        while (entry) { DictionaryEntry *next = entry->next; kfree(entry); entry = next; }
        dictionary->buckets[i] = 0;
    }
    dictionary->count = 0;
}
void dictionary_destroy(Dictionary *dictionary)
{
    if (dictionary) { dictionary_clear(dictionary); kfree(dictionary->buckets); kfree(dictionary); }
}
uint32_t dictionary_count(const Dictionary *dictionary) { return dictionary->count; }

bool dictionary_reserve(Dictionary *dictionary, uint32_t entries)
{
    uint32_t capacity = dictionary->capacity ? dictionary->capacity : 8u, i;
    DictionaryEntry **buckets;
    if (entries <= dictionary->capacity - dictionary->capacity / 4u) return true;
    /* Powers of two permit masking the hash; keep the table at most 75% full.
       Check the byte-size and doubling limits before allocating. */
    while (entries > capacity - capacity / 4u) {
        if (capacity > MAX_U32 / sizeof(*buckets) / 2u) return false;
        capacity *= 2u;
    }
    if (capacity > MAX_U32 / sizeof(*buckets)) return false;
    buckets = kmalloc(capacity * sizeof(*buckets));
    if (!buckets) return false;
    kmemset(buckets, 0, capacity * sizeof(*buckets));
    for (i = 0; i < dictionary->capacity; ++i) {
        DictionaryEntry *entry = dictionary->buckets[i];
        while (entry) {
            DictionaryEntry *next = entry->next;
            uint32_t index = entry->hash & (capacity - 1u);
            entry->next = buckets[index];
            buckets[index] = entry;
            entry = next;
        }
    }
    kfree(dictionary->buckets);
    dictionary->buckets = buckets;
    dictionary->capacity = capacity;
    return true;
}

static DictionaryEntry *dictionary_find(const Dictionary *dictionary, StringView key, uint32_t hash)
{
    DictionaryEntry *entry;
    if (!dictionary->capacity) return 0;
    for (entry = dictionary->buckets[hash & (dictionary->capacity - 1u)]; entry; entry = entry->next) {
        if (entry->hash == hash && string_view_equal(entry->key, key)) return entry;
    }
    return 0;
}

static DictionaryResult dictionary_put(Dictionary *dictionary, StringView key, void *value,
                                       bool replace, bool *out_replaced, void **out_previous)
{
    DictionaryEntry *entry;
    uint32_t hash, index;
    if (!dictionary || !view_valid(key)) return DICTIONARY_INVALID;
    /* Reject an impossible entry size before hashing or adding the terminator. */
    if (key.length > MAX_U32 - (uint32_t)sizeof(*entry) - 1u) return DICTIONARY_NO_MEMORY;
    hash = dictionary_hash(key);
    entry = dictionary_find(dictionary, key, hash);
    if (entry) {
        if (!replace) return DICTIONARY_EXISTS;
        if (out_replaced) *out_replaced = true;
        if (out_previous) *out_previous = entry->value;
        entry->value = value; /* The existing owned key remains unchanged. */
        return DICTIONARY_OK;
    }
    if (dictionary->count == MAX_U32 ||
        !(entry = kmalloc((uint32_t)sizeof(*entry) + key.length + 1u))) return DICTIONARY_NO_MEMORY;
    /* Copy before publishing: allocation or table growth failure must leave
       both the table and the caller's source bytes untouched. */
    kmemcpy_checked(entry->key_bytes, key.length, key.data, key.length);
    entry->key_bytes[key.length] = 0;
    entry->key = string_view(entry->key_bytes, key.length);
    entry->value = value;
    entry->hash = hash;
    if (!dictionary_reserve(dictionary, dictionary->count + 1u)) {
        kfree(entry);
        return DICTIONARY_NO_MEMORY;
    }
    index = hash & (dictionary->capacity - 1u);
    entry->next = dictionary->buckets[index];
    dictionary->buckets[index] = entry;
    ++dictionary->count;
    if (out_replaced) *out_replaced = false;
    if (out_previous) *out_previous = 0;
    return DICTIONARY_OK;
}
DictionaryResult dictionary_insert(Dictionary *dictionary, StringView key, void *value)
{ return dictionary_put(dictionary, key, value, false, 0, 0); }
DictionaryResult dictionary_set(Dictionary *dictionary, StringView key, void *value,
                                bool *out_replaced, void **out_previous)
{ return dictionary_put(dictionary, key, value, true, out_replaced, out_previous); }
bool dictionary_get(const Dictionary *dictionary, StringView key, void **out_value)
{
    DictionaryEntry *entry;
    if (!dictionary || !view_valid(key)) return false;
    entry = dictionary_find(dictionary, key, dictionary_hash(key));
    if (!entry) return false;
    if (out_value) *out_value = entry->value;
    return true;
}
bool dictionary_contains(const Dictionary *dictionary, StringView key)
{ return dictionary_get(dictionary, key, 0); }
bool dictionary_remove(Dictionary *dictionary, StringView key, void **out_value)
{
    uint32_t hash;
    DictionaryEntry **link, *entry;
    if (!dictionary || !view_valid(key) || !dictionary->capacity) return false;
    hash = dictionary_hash(key);
    link = &dictionary->buckets[hash & (dictionary->capacity - 1u)];
    while ((entry = *link) != 0) {
        if (entry->hash == hash && string_view_equal(entry->key, key)) {
            if (out_value) *out_value = entry->value;
            *link = entry->next;
            --dictionary->count;
            kfree(entry);
            return true;
        }
        link = &entry->next;
    }
    return false;
}
DictionaryIterator dictionary_iter_begin(const Dictionary *dictionary)
{
    DictionaryIterator iterator = {dictionary, 0, 0};
    return iterator;
}
bool dictionary_iter_next(DictionaryIterator *iterator, StringView *out_key, void **out_value)
{
    const Dictionary *dictionary = iterator->dictionary;
    const DictionaryEntry *entry = iterator->next;
    while (!entry && iterator->bucket < dictionary->capacity) entry = dictionary->buckets[iterator->bucket++];
    if (!entry) return false;
    iterator->next = entry->next;
    if (out_key) *out_key = entry->key;
    if (out_value) *out_value = entry->value;
    return true;
}
