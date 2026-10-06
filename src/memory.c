#include "memory.h"
#include "conio.h"

#define E820_MAX_ENTRIES 32u
#define MEMBLOCK_MAX_REGIONS 128u
#define MEMBLOCK_MAX_BOUNDARIES (2u * E820_MAX_ENTRIES + 2u)
#define FIRST_PAGE_END 4096ULL
#define DIRECT_PHYSICAL_END 0x100000000ULL
#define UINT64_MAX_VALUE (~(uint64_t)0)

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
/* GCC's asm names bypass MinGW's C underscore decoration. */
extern uint8_t kernel_start[] __asm__("__kernel_start");
extern uint8_t kernel_end[] __asm__("__kernel_end");

static MemblockRegion regions[MEMBLOCK_MAX_REGIONS];
static uint64_t boundaries[MEMBLOCK_MAX_BOUNDARIES];
static uint32_t region_count;
static uint32_t boundary_count;
static uint8_t initialized;

static __attribute__((noreturn)) void memblock_panic(const char *reason)
{
    kprintf("Memblock panic: %s\n", reason);
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

static uint64_t region_end(const MemblockRegion *region)
{
    return region->base + region->length;
}

static void validate_regions(void)
{
    uint32_t i;
    uint64_t previous_end = 0;

    if (region_count == 0 || region_count > MEMBLOCK_MAX_REGIONS ||
        regions[0].base != 0 || regions[0].type != MEMBLOCK_RESERVED ||
        regions[0].length < FIRST_PAGE_END) {
        memblock_panic("invalid first page or region count");
    }

    for (i = 0; i < region_count; ++i) {
        const MemblockRegion *region = &regions[i];
        if (region->length == 0 ||
            region->length > UINT64_MAX_VALUE - region->base ||
            (i != 0 && (region->base != previous_end ||
                         region->type == regions[i - 1].type))) {
            memblock_panic("invalid region ordering, extent, or merge");
        }
        previous_end = region_end(region);
    }
}

static void append_region(uint64_t base, uint64_t end, MemblockType type)
{
    MemblockRegion *last;

    if (end <= base) {
        memblock_panic("invalid region extent");
    }
    if (region_count != 0) {
        last = &regions[region_count - 1];
        if (region_end(last) > base) {
            memblock_panic("overlapping regions");
        }
        if (region_end(last) == base && last->type == type) {
            last->length += end - base;
            return;
        }
    }
    if (region_count == MEMBLOCK_MAX_REGIONS) {
        memblock_panic("region table full");
    }
    regions[region_count].base = base;
    regions[region_count].length = end - base;
    regions[region_count].type = type;
    ++region_count;
}

static void sort_boundaries(void)
{
    uint32_t i;

    for (i = 1; i < boundary_count; ++i) {
        uint64_t value = boundaries[i];
        uint32_t j = i;
        while (j != 0 && boundaries[j - 1] > value) {
            boundaries[j] = boundaries[j - 1];
            --j;
        }
        boundaries[j] = value;
    }
}

static MemblockType e820_type(uint32_t type, uint32_t *priority)
{
    switch (type) {
    case 1:
        *priority = 1;
        return MEMBLOCK_AVAILABLE;
    case 3:
        *priority = 2;
        return MEMBLOCK_ACPI_RECLAIMABLE;
    case 4:
        *priority = 3;
        return MEMBLOCK_ACPI_NVS;
    default:
        *priority = 4;
        return MEMBLOCK_RESERVED;
    }
}

/* Split only at an interior point; endpoints need no new region. */
static void split_at(uint64_t address)
{
    uint32_t i;

    for (i = 0; i < region_count; ++i) {
        uint64_t end = region_end(&regions[i]);
        uint32_t j;
        if (address <= regions[i].base || address >= end) {
            continue;
        }
        if (region_count == MEMBLOCK_MAX_REGIONS) {
            memblock_panic("region table full");
        }
        for (j = region_count; j > i + 1; --j) {
            regions[j] = regions[j - 1];
        }
        regions[i + 1] = regions[i];
        regions[i].length = address - regions[i].base;
        regions[i + 1].base = address;
        regions[i + 1].length = end - address;
        ++region_count;
        return;
    }
}

static void merge_regions(void)
{
    uint32_t i = 1;

    while (i < region_count) {
        if (region_end(&regions[i - 1]) == regions[i].base &&
            regions[i - 1].type == regions[i].type) {
            uint32_t j;
            regions[i - 1].length += regions[i].length;
            for (j = i; j + 1 < region_count; ++j) {
                regions[j] = regions[j + 1];
            }
            --region_count;
        } else {
            ++i;
        }
    }
}

/* Both kernel and boot allocations must be fully covered by available RAM. */
static void reserve_available(uint64_t base, uint64_t end, MemblockType type)
{
    uint64_t covered = base;
    uint32_t i;

    if (end <= base || (type != MEMBLOCK_KERNEL &&
                        type != MEMBLOCK_BOOT_ALLOCATED)) {
        memblock_panic("invalid reservation");
    }
    for (i = 0; i < region_count && covered < end; ++i) {
        uint64_t next = region_end(&regions[i]);
        if (next <= covered) {
            continue;
        }
        if (regions[i].base > covered || regions[i].type != MEMBLOCK_AVAILABLE) {
            memblock_panic("reservation is not available RAM");
        }
        covered = next < end ? next : end;
    }
    if (covered != end) {
        memblock_panic("reservation extends beyond known RAM");
    }

    split_at(base);
    split_at(end);
    for (i = 0; i < region_count; ++i) {
        if (regions[i].base >= end) {
            break;
        }
        if (regions[i].base >= base) {
            regions[i].type = type;
        }
    }
    merge_regions();
    validate_regions();
}

void memblock_init(void)
{
    uint32_t i;

    if (initialized || e820_detection_failed ||
        e820_entry_count == 0 || e820_entry_count > E820_MAX_ENTRIES) {
        memblock_panic("E820 map missing, incomplete, or already imported");
    }

    region_count = 0;
    boundary_count = 0;
    boundaries[boundary_count++] = 0;
    boundaries[boundary_count++] = FIRST_PAGE_END;
    for (i = 0; i < e820_entry_count; ++i) {
        uint64_t base = e820_map[i].base_address;
        uint64_t length = e820_map[i].length;
        if (length == 0) {
            continue;
        }
        if (length > UINT64_MAX_VALUE - base) {
            memblock_panic("E820 address overflow");
        }
        boundaries[boundary_count++] = base;
        boundaries[boundary_count++] = base + length;
    }
    sort_boundaries();

    for (i = 1; i < boundary_count; ++i) {
        uint64_t base = boundaries[i - 1];
        uint64_t end = boundaries[i];
        uint32_t j;
        uint32_t best_priority = 0;
        MemblockType type = MEMBLOCK_UNAVAILABLE;

        if (end == base) {
            continue;
        }
        for (j = 0; j < e820_entry_count; ++j) {
            uint32_t priority;
            MemblockType candidate;
            uint64_t source_end = e820_map[j].base_address + e820_map[j].length;
            if (e820_map[j].length == 0 ||
                e820_map[j].base_address > base || source_end < end) {
                continue;
            }
            candidate = e820_type(e820_map[j].type, &priority);
            if (priority > best_priority) {
                best_priority = priority;
                type = candidate;
            }
        }
        if (base < FIRST_PAGE_END) {
            type = MEMBLOCK_RESERVED;
        }
        append_region(base, end, type);
    }
    validate_regions();

    /* The linker bounds include image, .bss, alignment padding, and stack. */
    reserve_available((uint32_t)kernel_start, (uint32_t)kernel_end,
                      MEMBLOCK_KERNEL);
    initialized = 1;
}

uint64_t memblock_alloc(uint64_t size, uint64_t alignment)
{
    uint32_t i;
    uint64_t mask;

    if (!initialized || size == 0 || alignment == 0 ||
        (alignment & (alignment - 1)) != 0) {
        memblock_panic("invalid allocation request or uninitialized map");
    }
    mask = alignment - 1;
    for (i = 0; i < region_count; ++i) {
        uint64_t base;
        uint64_t end;
        if (regions[i].type != MEMBLOCK_AVAILABLE) {
            continue;
        }
        if (regions[i].base >= DIRECT_PHYSICAL_END) {
            break;
        }
        end = region_end(&regions[i]);
        if (end > DIRECT_PHYSICAL_END) {
            end = DIRECT_PHYSICAL_END;
        }
        base = regions[i].base;
        if (base < FIRST_PAGE_END) {
            base = FIRST_PAGE_END;
        }
        if (base > UINT64_MAX_VALUE - mask) {
            continue;
        }
        base = (base + mask) & ~mask;
        if (base < end && size <= end - base) {
            reserve_available(base, base + size, MEMBLOCK_BOOT_ALLOCATED);
            return base;
        }
    }
    return 0;
}

uint32_t memblock_region_count(void)
{
    if (!initialized) {
        memblock_panic("map not initialized");
    }
    return region_count;
}

const MemblockRegion *memblock_region(uint32_t index)
{
    if (!initialized) {
        memblock_panic("map not initialized");
    }
    return index < region_count ? &regions[index] : 0;
}

static const char *memblock_type_name(MemblockType type)
{
    switch (type) {
    case MEMBLOCK_UNAVAILABLE: return "UNAVAILABLE";
    case MEMBLOCK_AVAILABLE: return "AVAILABLE";
    case MEMBLOCK_RESERVED: return "RESERVED";
    case MEMBLOCK_KERNEL: return "KERNEL";
    case MEMBLOCK_BOOT_ALLOCATED: return "BOOT_ALLOCATED";
    case MEMBLOCK_ACPI_RECLAIMABLE: return "ACPI_RECLAIMABLE";
    case MEMBLOCK_ACPI_NVS: return "ACPI_NVS";
    default: return "INVALID";
    }
}

void memblock_dump(void)
{
    uint32_t i;
    if (!initialized) {
        memblock_panic("map not initialized");
    }
    kputs("XNF physical memory map [base, end):\n");
    for (i = 0; i < region_count; ++i) {
        kprintf("%X-%X  %s\n", regions[i].base, region_end(&regions[i]),
                memblock_type_name(regions[i].type));
    }
}

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
