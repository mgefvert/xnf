/*
 * Shared C layout of BIOS E820 records collected by real-mode assembly.
 * Legacy attributes are normalized during collection; disabled extended entries are ignored.
 */

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
/* These buffers are in the loaded image, not .bss: they must survive the later
   zeroing pass and be readable during the pre-.bss safety check. */
extern uint16_t e820_entry_count;
extern uint8_t e820_detection_failed;
extern E820Entry e820_map[E820_MAX_ENTRIES];

#endif
