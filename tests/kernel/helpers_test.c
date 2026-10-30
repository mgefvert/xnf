/*
 * Test-only coverage for helper ownership, bounds, growth, and failure rollback.
 * Host QEMU harness selects this entry after boot; normal images exclude it.
 */
#include "helpers.h"
#include "kheap.h"
#include "kernel_test.h"
#include "panic.h"
#include "conio.h"
#include "vmm.h"
#include "pmm.h"

volatile uint32_t helpers_test_stage;
static char keys[64][4];
static void *pressure_slots[2048];

static void require(bool condition, const char *reason)
{
    if (!condition) kernel_panic("Helpers test", reason);
}

void helpers_self_test(void)
{
    uint32_t allocations = kheap_live_allocations(), pages = kheap_allocated_pages();
    uint32_t physical = pmm_free_pages(), virtual = vmm_free_page_count();
    char bytes[16] = "abcdef";
    char mutable_key[] = "driver";
    char binary_key[] = {'x', 0, 'y'};
    int values[64];
    uint32_t i, count, available;
    void *value, *previous, *pressure;
    bool replaced;
    KString *string;
    List *list;
    LinkedList *linked, *other;
    LinkedListNode *first, *middle, *last;
    Dictionary *dictionary, *full_dictionary;
    DictionaryIterator iterator;
    StringView key;

    helpers_test_stage = 1;
    require(kstrlen("abc") == 3 && kstrnlen("abc", 2) == 2 &&
            kstrcmp("a", "b") < 0 && kstrncmp("abc", "abd", 2) == 0,
            "string primitives");
    require(kstrchr(bytes, 'c') == bytes + 2 && kstrrchr(bytes, 'a') == bytes &&
            kstrchr(bytes, 0) == bytes + 6, "character search");
    require(!kmemcpy_checked(bytes, 2, "xyz", 3) && kstrcmp(bytes, "abcdef") == 0,
            "bounded copy modified destination");
    require(kmemmove_checked(bytes + 1, 15, bytes, 5) && kstrncmp(bytes, "aabcde", 6) == 0,
            "overlap move");
    require(kmemmove_checked(bytes, 16, bytes + 1, 5) && kstrncmp(bytes, "abcde", 5) == 0,
            "reverse overlap move");
    require(kstr_copy(bytes, sizeof(bytes), STRING_VIEW_LITERAL("hello")) &&
            kstr_append(bytes, sizeof(bytes), string_view(bytes, 5)) &&
            kstrcmp(bytes, "hellohello") == 0 &&
            !kstr_append(bytes, 10, STRING_VIEW_LITERAL("x")) &&
            kstrcmp(bytes, "hellohello") == 0, "bounded string copy/append");

    string = kstring_create(STRING_VIEW_LITERAL("abc"));
    require(string && kstring_length(string) == 3 && kstring_capacity(string) == 3, "string create");
    require(kstring_append(&string, kstring_view(string)) &&
            string_view_equal(kstring_view(string), STRING_VIEW_LITERAL("abcabc")), "self append growth");
    require(kstring_reserve(&string, 32) && kstring_insert(&string, 1, string_view(kstring_data(string) + 2, 3)) &&
            string_view_equal(kstring_view(string), STRING_VIEW_LITERAL("acabbcabc")), "aliased insertion");
    require(kstring_assign(&string, string_view(kstring_data(string) + 6, 3)) &&
            string_view_equal(kstring_view(string), STRING_VIEW_LITERAL("abc")), "aliased assign");
    require(!kstring_insert(&string, 4, STRING_VIEW_LITERAL("x")) &&
            !kstring_reserve(&string, 0xFFFFFFFFu) && kstrcmp(kstring_data(string), "abc") == 0,
            "string bounds rollback");
    require(kstring_assign(&string, string_view(0, 0)) && kstring_length(string) == 0 &&
            kstring_data(string)[0] == 0, "empty string");

    list = list_create();
    require(list && !list_pop(list, &value), "empty list");
    for (i = 0; i < 64; ++i) { values[i] = (int)i; require(list_append(list, &values[i]), "list append"); }
    require(list_capacity(list) == 64 && list_count(list) == 64 &&
            list_insert(list, 1, 0) && list_capacity(list) == 128, "list doubling");
    require(list_get(list, 1, &value) && !value && list_remove(list, 1, &value) && !value &&
            list_get(list, 1, &value) && value == &values[1], "list shift/null");
    require(list_set(list, 1, &values[3], &previous) && previous == &values[1] &&
            !list_get(list, 64, &value) && !list_reserve(list, 0xFFFFFFFFu), "list bounds");
    require(list_shrink_to_fit(list) && list_capacity(list) == 64, "list shrink");
    list_clear(list);
    require(list_capacity(list) == 64 && list_shrink_to_fit(list) && list_capacity(list) == 0,
            "list clear/shrink");

    linked = linked_list_create(); other = linked_list_create();
    require(linked && other, "linked create");
    first = linked_list_push_front(linked, &values[0]);
    last = linked_list_push_back(linked, &values[2]);
    middle = linked_list_insert_before(linked, last, 0);
    require(first && middle && last && linked_list_count(linked) == 3 &&
            linked_list_next(first) == middle && linked_list_previous(last) == middle &&
            linked_list_value(middle) == 0 && !linked_list_remove(other, middle, 0), "linked traversal");
    require(linked_list_remove(linked, middle, &value) && !value &&
            linked_list_pop_front(linked, &value) && value == &values[0] &&
            linked_list_pop_back(linked, &value) && value == &values[2] &&
            !linked_list_pop_back(linked, 0), "linked removal");
    require(linked_list_push_back(linked, &values[0]) &&
            linked_list_insert_after(linked, linked_list_first(linked), &values[1]), "linked insert after");
    linked_list_clear(linked);
    require(!linked_list_first(linked) && !linked_list_last(linked), "linked clear");

    dictionary = dictionary_create();
    require(dictionary && dictionary_insert(dictionary, STRING_VIEW_LITERAL("a"), 0) == DICTIONARY_OK &&
            dictionary_insert(dictionary, STRING_VIEW_LITERAL("i"), &values[1]) == DICTIONARY_OK &&
            dictionary_get(dictionary, STRING_VIEW_LITERAL("a"), &value) && !value &&
            dictionary_get(dictionary, STRING_VIEW_LITERAL("i"), &value) && value == &values[1],
            "dictionary collision/null");
    require(dictionary_insert(dictionary, STRING_VIEW_LITERAL("a"), &values[0]) == DICTIONARY_EXISTS &&
            dictionary_set(dictionary, STRING_VIEW_LITERAL("a"), &values[0], &replaced, &previous) == DICTIONARY_OK &&
            replaced && !previous, "dictionary replace");
    /* These distinct strings have the same full FNV-1a hash, not just the
       same bucket, so equality must still compare the actual bytes. */
    require(dictionary_insert(dictionary, STRING_VIEW_LITERAL("costarring"), &values[2]) == DICTIONARY_OK &&
            dictionary_insert(dictionary, STRING_VIEW_LITERAL("liquid"), &values[3]) == DICTIONARY_OK &&
            dictionary_get(dictionary, STRING_VIEW_LITERAL("costarring"), &value) && value == &values[2] &&
            dictionary_remove(dictionary, STRING_VIEW_LITERAL("liquid"), &value) && value == &values[3] &&
            dictionary_remove(dictionary, STRING_VIEW_LITERAL("costarring"), &value) && value == &values[2],
            "dictionary full hash collision");
    require(dictionary_set(dictionary, string_view(0, 0), 0, &replaced, &previous) == DICTIONARY_OK &&
            !replaced && !previous && dictionary_contains(dictionary, STRING_VIEW_LITERAL("")) &&
            dictionary_remove(dictionary, STRING_VIEW_LITERAL(""), &value) && !value,
            "dictionary empty key");
    /* Source storage is independent from the owned key, including embedded
       zeros. Replacements preserve the existing entry and its owned bytes. */
    require(dictionary_insert(dictionary, string_view_cstr(mutable_key), &values[4]) == DICTIONARY_OK,
            "owned key insert");
    mutable_key[0] = 'D';
    require(dictionary_get(dictionary, STRING_VIEW_LITERAL("driver"), &value) && value == &values[4] &&
            !dictionary_contains(dictionary, string_view_cstr(mutable_key)) &&
            dictionary_set(dictionary, STRING_VIEW_LITERAL("driver"), &values[5], &replaced, &previous) == DICTIONARY_OK &&
            replaced && previous == &values[4], "owned key mutation/replace");
    iterator = dictionary_iter_begin(dictionary);
    while (dictionary_iter_next(&iterator, &key, &value)) {
        if (string_view_equal(key, STRING_VIEW_LITERAL("driver"))) break;
    }
    require(key.data != mutable_key && string_view_equal(key, STRING_VIEW_LITERAL("driver")) &&
            key.data[key.length] == 0 && value == &values[5], "owned key iteration");
    require(dictionary_reserve(dictionary, 32) && string_view_equal(key, STRING_VIEW_LITERAL("driver")),
            "owned key survives rehash");
    i = kheap_live_allocations();
    require(dictionary_remove(dictionary, STRING_VIEW_LITERAL("driver"), &value) && value == &values[5] &&
            kheap_live_allocations() + 1u == i, "owned key removal");
    require(dictionary_insert(dictionary, string_view(binary_key, sizeof(binary_key)), &values[6]) == DICTIONARY_OK,
            "binary key insert");
    binary_key[0] = 'z';
    require(dictionary_get(dictionary, string_view("x\0y", 3), &value) && value == &values[6] &&
            dictionary_remove(dictionary, string_view("x\0y", 3), 0), "owned binary key");
    require(kstring_assign(&string, STRING_VIEW_LITERAL("managed")) &&
            dictionary_insert(dictionary, kstring_view(string), &values[7]) == DICTIONARY_OK &&
            kstring_assign(&string, STRING_VIEW_LITERAL("changed")), "managed key fixture");
    kstring_destroy(string);
    require(dictionary_get(dictionary, STRING_VIEW_LITERAL("managed"), &value) && value == &values[7] &&
            dictionary_remove(dictionary, STRING_VIEW_LITERAL("managed"), 0), "destroyed source key");
    string = kstring_create(STRING_VIEW_LITERAL("abc"));
    require(string != 0, "recreate managed string");
    replaced = true; previous = &values[0];
    require(dictionary_set(dictionary, string_view("x", 0xFFFFFFFFu), &values[0], &replaced, &previous) ==
            DICTIONARY_NO_MEMORY && replaced && previous == &values[0], "key size overflow rollback");
    for (i = 0; i < 64; ++i) {
        keys[i][0] = 'k'; keys[i][1] = (char)('0' + i / 10u); keys[i][2] = (char)('0' + i % 10u); keys[i][3] = 0;
        require(dictionary_insert(dictionary, string_view(keys[i], 3), &values[i]) == DICTIONARY_OK,
                "dictionary growth");
    }
    for (i = 0; i < 64; ++i) {
        require(dictionary_get(dictionary, string_view(keys[i], 3), &value) && value == &values[i],
                "dictionary lookup after rehash");
    }
    require(!dictionary_reserve(dictionary, 0xFFFFFFFFu) && dictionary_count(dictionary) == 66,
            "dictionary reserve rollback");
    iterator = dictionary_iter_begin(dictionary); count = 0;
    while (dictionary_iter_next(&iterator, &key, &value)) { require(key.data != 0, "iteration key"); ++count; }
    require(count == 66 && dictionary_remove(dictionary, STRING_VIEW_LITERAL("i"), &value) &&
            value == &values[1] && !dictionary_contains(dictionary, STRING_VIEW_LITERAL("i")),
            "dictionary iteration/remove");
    require(kstring_assign(&string, STRING_VIEW_LITERAL("abc")) && kstring_reserve(&string, 32),
            "pressure string fixture");

    /* Six entries fill the initial table to its 75% threshold. The next
       insertion needs one small entry and a larger replacement bucket array. */
    full_dictionary = dictionary_create();
    require(full_dictionary &&
            dictionary_insert(full_dictionary, STRING_VIEW_LITERAL("0"), &values[0]) == DICTIONARY_OK &&
            dictionary_insert(full_dictionary, STRING_VIEW_LITERAL("1"), &values[1]) == DICTIONARY_OK &&
            dictionary_insert(full_dictionary, STRING_VIEW_LITERAL("2"), &values[2]) == DICTIONARY_OK &&
            dictionary_insert(full_dictionary, STRING_VIEW_LITERAL("3"), &values[3]) == DICTIONARY_OK &&
            dictionary_insert(full_dictionary, STRING_VIEW_LITERAL("4"), &values[4]) == DICTIONARY_OK &&
            dictionary_insert(full_dictionary, STRING_VIEW_LITERAL("5"), &values[5]) == DICTIONARY_OK,
            "growth rollback fixture");

    /* Reserve all currently free pages, then consume the existing heap holes.
       This forces genuine heap allocation failures without corrupting objects. */
    available = pmm_free_pages();
    if (available > vmm_free_page_count()) available = vmm_free_page_count();
    pressure = vmm_alloc_pages(available);
    require(pressure != 0, "pressure reservation");
    count = 0;
    while (count < 2048 && (pressure_slots[count] = kmalloc(32)) != 0) ++count;
    require(count > 2 && count < 2048, "pressure exhausted fixture slots");
    /* Free a block bracketed by live blocks. The entry fits there, but the
       new bucket array does not; failed growth must free the private entry. */
    i = count - 2u;
    kfree(pressure_slots[i]);
    pressure_slots[i] = 0;
    require(kmalloc(64) == 0, "isolated growth rollback hole");
    available = kheap_live_allocations();
    require(dictionary_insert(full_dictionary, STRING_VIEW_LITERAL("6"), &values[6]) == DICTIONARY_NO_MEMORY &&
            dictionary_count(full_dictionary) == 6 && kheap_live_allocations() == available &&
            dictionary_get(full_dictionary, STRING_VIEW_LITERAL("0"), &value) && value == &values[0],
            "owned entry table growth rollback");
    pressure_slots[i] = kmalloc(32);
    require(pressure_slots[i] != 0, "reclaim isolated pressure hole");
    require(!kstring_reserve(&string, 100000) &&
            !kstring_insert(&string, 1, kstring_view(string)) && kstrcmp(kstring_data(string), "abc") == 0 &&
            !list_reserve(list, 100000) && !list_append(list, &values[0]) && list_count(list) == 0 &&
            dictionary_insert(dictionary, STRING_VIEW_LITERAL("new"), &values[0]) == DICTIONARY_NO_MEMORY &&
            !dictionary_reserve(dictionary, 100000) && dictionary_count(dictionary) == 65 &&
            dictionary_get(dictionary, string_view(keys[42], 3), &value) && value == &values[42],
            "real exhaustion rollback");
    while (count) kfree(pressure_slots[--count]);
    require(vmm_free_pages(pressure), "pressure release");
    i = kheap_live_allocations();
    count = dictionary_count(dictionary);
    dictionary_clear(dictionary);
    require(dictionary_count(dictionary) == 0 && kheap_live_allocations() + count == i && values[42] == 42,
            "dictionary clear releases keys, preserves values");
    i = kheap_live_allocations();
    dictionary_destroy(full_dictionary);
    require(kheap_live_allocations() + 8u == i, "dictionary destroy releases keys and table");
    dictionary_destroy(dictionary); linked_list_destroy(other); linked_list_destroy(linked);
    list_destroy(list); kstring_destroy(string);
    require(kheap_live_allocations() == allocations && kheap_allocated_pages() == pages &&
            pmm_free_pages() == physical && vmm_free_page_count() == virtual, "ownership leaked");
    kputs("Helper tests passed: strings/containers/pressure\n");
    helpers_test_stage = 2;
}

void helpers_test_entry(void)
{
    extern void interrupts_enable_and_halt(void);
    helpers_self_test();
    for (;;) interrupts_enable_and_halt();
}
