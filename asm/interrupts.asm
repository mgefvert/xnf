; First protected-mode exception path. These handlers run in ring 0 with the
; flat kernel segments already loaded by protected_mode_entry.

BITS 32

KERNEL_CODE_SELECTOR equ 0x08
IDT_ENTRY_COUNT equ 256
EXCEPTION_COUNT equ 32

extern _exception_handler
global _interrupts_init

section .text

; Exceptions without a processor-pushed error code get a synthetic zero.
%macro ISR_NO_ERROR 1
isr%1:
    push dword 0
    push dword %1
    jmp exception_common
%endmacro

; Exceptions with a processor-pushed error code only add the vector number.
%macro ISR_WITH_ERROR 1
isr%1:
    push dword %1
    jmp exception_common
%endmacro

ISR_NO_ERROR 0
ISR_NO_ERROR 1
ISR_NO_ERROR 2
ISR_NO_ERROR 3
ISR_NO_ERROR 4
ISR_NO_ERROR 5
ISR_NO_ERROR 6
ISR_NO_ERROR 7
ISR_WITH_ERROR 8
ISR_NO_ERROR 9
ISR_WITH_ERROR 10
ISR_WITH_ERROR 11
ISR_WITH_ERROR 12
ISR_WITH_ERROR 13
ISR_WITH_ERROR 14
ISR_NO_ERROR 15
ISR_NO_ERROR 16
ISR_WITH_ERROR 17
ISR_NO_ERROR 18
ISR_NO_ERROR 19
ISR_NO_ERROR 20
ISR_WITH_ERROR 21
ISR_NO_ERROR 22
ISR_NO_ERROR 23
ISR_NO_ERROR 24
ISR_NO_ERROR 25
ISR_NO_ERROR 26
ISR_NO_ERROR 27
ISR_NO_ERROR 28
ISR_WITH_ERROR 29
ISR_WITH_ERROR 30
ISR_NO_ERROR 31

; At entry the normalized stack is vector, error code, EIP, CS, EFLAGS.
; PUSHAD makes the structure passed to C begin with EDI, ESI, EBP, saved ESP,
; EBX, EDX, ECX, and EAX. Clear DF before entering C as required by its ABI.
exception_common:
    pushad
    cld

    ; Keep a pointer to the saved frame and align the C call site to 16 bytes.
    mov ebp, esp
    and esp, 0xFFFFFFF0
    sub esp, 12
    push ebp
    call _exception_handler

    ; The C handler currently halts. These instructions make the common path
    ; well-formed if it is later changed to return.
    mov esp, ebp
    popad
    add esp, 8                 ; Discard vector and normalized error code.
    iretd

; void interrupts_init(void)
_interrupts_init:
    pushad
    cld

    ; Clear all 256 descriptors before installing the CPU exception gates.
    mov edi, idt
    xor eax, eax
    mov ecx, IDT_ENTRY_COUNT * 2
    rep stosd

    ; Each gate is a present, ring-0, 32-bit interrupt gate (type 0xE).
    mov esi, isr_stub_table
    mov edi, idt
    mov ecx, EXCEPTION_COUNT
.install_exception_gate:
    lodsd
    mov [edi], ax              ; Handler offset bits 0..15.
    mov word [edi + 2], KERNEL_CODE_SELECTOR
    mov byte [edi + 4], 0      ; Reserved byte.
    mov byte [edi + 5], 0x8E   ; P=1, DPL=0, 32-bit interrupt gate.
    shr eax, 16
    mov [edi + 6], ax          ; Handler offset bits 16..31.
    add edi, 8
    loop .install_exception_gate

    lidt [idtr]
    popad
    ret

section .rodata
align 4, db 0
isr_stub_table:
    dd isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7
    dd isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15
    dd isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
    dd isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31

section .data
align 4, db 0
idtr:
    dw IDT_ENTRY_COUNT * 8 - 1
    dd idt

section .bss
align 8
idt:
    resb IDT_ENTRY_COUNT * 8
