#include "types.h"
#include "conio.h"

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
    uint32_t fault_address = 0;

    if (frame->vector == 14) {
        /* Capture CR2 before console output could cause another page fault. */
        __asm__ volatile ("mov %%cr2, %0" : "=r" (fault_address) : : "memory");
    }

    kprintf("Exception %d: %s\n", frame->vector, exception_name(frame->vector));
    kprintf("Error code: %x\n", frame->error_code);
    kprintf("EIP: %x  CS: %x  EFLAGS: %x\n",
            frame->eip, frame->cs, frame->eflags);

    if (frame->vector == 14) {
        kprintf("Faulting linear address (CR2): %x\n", fault_address);
        kprintf("Page fault: %s, %s, %s\n",
                (frame->error_code & (1u << 0)) ? "protection violation" : "non-present page",
                (frame->error_code & (1u << 1)) ? "write" : "read",
                (frame->error_code & (1u << 2)) ? "user" : "supervisor");
        kprintf("Reserved-bit violation: %s  Instruction fetch: %s\n",
                (frame->error_code & (1u << 3)) ? "yes" : "no",
                (frame->error_code & (1u << 4)) ? "yes" : "no");
    }

    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}
