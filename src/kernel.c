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

static void display_e820_map(void)
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

/*
 * Stack frame passed by exception_common after PUSHAD. PUSHAD stores EDI
 * first at the lowest address, followed by ESI, EBP, its saved ESP, EBX,
 * EDX, ECX, and EAX. The normalized vector/error pair follows, then the
 * processor's EIP, CS, and EFLAGS frame.
 */
typedef struct ExceptionFrame {
    uint32_t edi;
    uint32_t esi;
    uint32_t ebp;
    uint32_t saved_esp;
    uint32_t ebx;
    uint32_t edx;
    uint32_t ecx;
    uint32_t eax;
    uint32_t vector;
    uint32_t error_code;
    uint32_t eip;
    uint32_t cs;
    uint32_t eflags;
} ExceptionFrame;

static const char *exception_name(uint32_t vector)
{
    static const char *const names[32] = {
        "Divide Error", "Debug", "Non-maskable Interrupt", "Breakpoint",
        "Overflow", "BOUND Range Exceeded", "Invalid Opcode",
        "Device Not Available", "Double Fault", "Coprocessor Segment Overrun",
        "Invalid TSS", "Segment Not Present", "Stack-Segment Fault",
        "General Protection Fault", "Page Fault", "Reserved",
        "x87 Floating-Point Exception", "Alignment Check", "Machine Check",
        "SIMD Floating-Point Exception", "Virtualization Exception",
        "Control Protection Exception", "Reserved", "Reserved", "Reserved",
        "Reserved", "Reserved", "Reserved", "Hypervisor Injection Exception",
        "VMM Communication Exception", "Security Exception", "Reserved"
    };

    return vector < 32 ? names[vector] : "Unknown Exception";
}

void exception_handler(const ExceptionFrame *frame)
{
    kprintf("Exception %d: %s\n", frame->vector, exception_name(frame->vector));
    kprintf("Error code: %x\n", frame->error_code);
    kprintf("EIP: %x  CS: %x  EFLAGS: %x\n",
            frame->eip, frame->cs, frame->eflags);

    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

void kernel_main(void)
{
    clear_screen();
    display_e820_map();
}
