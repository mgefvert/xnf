/*
 * Freestanding strings and pointer containers backed by the kernel heap.
 * See docs/HELPERS.md for ownership, failure and execution-context contracts.
 */
#ifndef XNF_HELPERS_H
#define XNF_HELPERS_H

#include <stdbool.h>
#include "types.h"

typedef struct KString KString;
typedef struct StringView { const char *data; uint32_t length; } StringView;

/* Views borrow bytes; NULL is valid only for a zero-length view. */
StringView string_view(const char *data, uint32_t length);
StringView string_view_cstr(const char *string);
#define STRING_VIEW_LITERAL(s) ((StringView){(s), sizeof(s) - 1u})
uint32_t kstrnlen(const char *string, uint32_t limit);
uint32_t kstrlen(const char *string);
int kstrcmp(const char *left, const char *right);
int kstrncmp(const char *left, const char *right, uint32_t limit);
const char *kstrchr(const char *string, int character);
const char *kstrrchr(const char *string, int character);
int kmemcmp(const void *left, const void *right, uint32_t size);
void *kmemset(void *destination, int value, uint32_t size);
/* Return false without writing when size exceeds destination capacity.
   copy requires nonoverlapping regions; move supports overlap. */
bool kmemcpy_checked(void *destination, uint32_t capacity, const void *source, uint32_t size);
bool kmemmove_checked(void *destination, uint32_t capacity, const void *source, uint32_t size);
bool string_view_equal(StringView left, StringView right);
/* Copy or append a view to a C buffer, including a terminator. On failure the
   buffer is unchanged; appending requires an existing terminator in capacity. */
bool kstr_copy(char *destination, uint32_t capacity, StringView source);
bool kstr_append(char *destination, uint32_t capacity, StringView source);

KString *kstring_create(StringView initial);
void kstring_destroy(KString *string);
uint32_t kstring_length(const KString *string);
uint32_t kstring_capacity(const KString *string);
const char *kstring_data(const KString *string);
StringView kstring_view(const KString *string);
/* Mutators may move *string. Views into it remain valid only until a
   successful mutation. Input views may refer to its own character bytes. */
bool kstring_reserve(KString **string, uint32_t capacity);
bool kstring_assign(KString **string, StringView value);
bool kstring_append(KString **string, StringView suffix);
bool kstring_insert(KString **string, uint32_t index, StringView value);

typedef struct List List;
List *list_create(void);
void list_destroy(List *list);
void list_clear(List *list);
uint32_t list_count(const List *list);
uint32_t list_capacity(const List *list);
bool list_reserve(List *list, uint32_t capacity);
bool list_shrink_to_fit(List *list);
bool list_append(List *list, void *value);
bool list_insert(List *list, uint32_t index, void *value);
bool list_get(const List *list, uint32_t index, void **out_value);
bool list_set(List *list, uint32_t index, void *value, void **out_previous);
bool list_remove(List *list, uint32_t index, void **out_value);
bool list_pop(List *list, void **out_value);

typedef struct LinkedList LinkedList;
/* Embed as the first member of a caller-owned object. The first two pointers
   in that object are the previous and next nodes, in that order. Initialize
   both to NULL before insertion; removal and clear reset them to NULL. */
typedef struct LinkedListNode {
    struct LinkedListNode *previous;
    struct LinkedListNode *next;
} LinkedListNode;
LinkedList *linked_list_create(void);
void linked_list_destroy(LinkedList *list);
void linked_list_clear(LinkedList *list);
uint32_t linked_list_count(const LinkedList *list);
LinkedListNode *linked_list_first(const LinkedList *list);
LinkedListNode *linked_list_last(const LinkedList *list);
LinkedListNode *linked_list_next(const LinkedListNode *node);
LinkedListNode *linked_list_previous(const LinkedListNode *node);
/* Nodes must be detached and belong to at most one list. An anchor or node
   passed for removal must belong to list. These membership preconditions
   cannot be fully checked from two links, notably for a singleton list. */
bool linked_list_push_front(LinkedList *list, LinkedListNode *node);
bool linked_list_push_back(LinkedList *list, LinkedListNode *node);
bool linked_list_insert_before(LinkedList *list, LinkedListNode *anchor, LinkedListNode *node);
bool linked_list_insert_after(LinkedList *list, LinkedListNode *anchor, LinkedListNode *node);
bool linked_list_remove(LinkedList *list, LinkedListNode *node);
LinkedListNode *linked_list_pop_front(LinkedList *list);
LinkedListNode *linked_list_pop_back(LinkedList *list);

typedef struct Dictionary Dictionary;
typedef enum DictionaryResult {
    DICTIONARY_OK, DICTIONARY_EXISTS, DICTIONARY_NO_MEMORY, DICTIONARY_INVALID
} DictionaryResult;
typedef struct DictionaryIterator {
    const Dictionary *dictionary;
    uint32_t bucket;
    const void *next;
} DictionaryIterator;
/* Insertion copies key bytes into heap-owned storage. Input views need only
   remain valid for the call; values remain borrowed and are never freed. */
Dictionary *dictionary_create(void);
void dictionary_destroy(Dictionary *dictionary);
void dictionary_clear(Dictionary *dictionary);
uint32_t dictionary_count(const Dictionary *dictionary);
bool dictionary_reserve(Dictionary *dictionary, uint32_t entries);
DictionaryResult dictionary_insert(Dictionary *dictionary, StringView key, void *value);
/* Set reports whether an entry existed and optionally its previous value. */
DictionaryResult dictionary_set(Dictionary *dictionary, StringView key, void *value,
                                bool *out_replaced, void **out_previous);
bool dictionary_get(const Dictionary *dictionary, StringView key, void **out_value);
bool dictionary_contains(const Dictionary *dictionary, StringView key);
bool dictionary_remove(Dictionary *dictionary, StringView key, void **out_value);
DictionaryIterator dictionary_iter_begin(const Dictionary *dictionary);
/* Iterated key views borrow dictionary storage and expire when the entry is
   removed, the dictionary is cleared, or it is destroyed. Structural changes
   invalidate the iterator; replacing an existing value does not. */
bool dictionary_iter_next(DictionaryIterator *iterator, StringView *out_key, void **out_value);

#endif
