#ifndef XNF_E820_H
#define XNF_E820_H

#include "types.h"

#define E820_MAX_ENTRIES 32u

typedef struct E820Entry {
    uint64_t base_address;
    uint64_t length;
    uint32_t type;
    uint32_t attributes; /* Legacy 20-byte responses are normalized to enabled. */
} E820Entry;

_Static_assert(sizeof(E820Entry) == 24, "E820 entry must match assembly");
extern uint16_t e820_entry_count;
extern uint8_t e820_detection_failed;
extern E820Entry e820_map[E820_MAX_ENTRIES];

#endif
