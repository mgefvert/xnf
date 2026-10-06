#include "types.h"
#include "conio.h"

#define E820_MAX_ENTRIES 32u

typedef struct E820Entry {
    unsigned long long base_address;
    unsigned long long length;
    uint32_t type;
    uint32_t attributes;
} E820Entry;

typedef char E820Entry_must_be_24_bytes[(sizeof(E820Entry) == 24) ? 1 : -1];

extern uint16_t e820_entry_count;
extern uint8_t e820_detection_failed;
extern E820Entry e820_map[E820_MAX_ENTRIES];

void display_e820_map(void)
{
    uint32_t index;
    const char *collection_status = e820_detection_failed
        ? " (collection incomplete)"
        : "";

    kprintf("E820 memory map: %d entries%s\n",
            (uint32_t)e820_entry_count, collection_status);

    for (index = 0; index < e820_entry_count; ++index) {
        const E820Entry *entry = &e820_map[index];
        kprintf("Entry %d base=%X length=%X\n       type=%x attributes=%x\n",
                index,
                entry->base_address,
                entry->length,
                entry->type,
                entry->attributes);
    }
}
