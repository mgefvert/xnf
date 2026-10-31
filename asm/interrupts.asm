; -----------------------------------------------------------------------------
; Hardware entry/exit for CPU exceptions and the timer IRQ.
; Builds the IDT, configures the PIC and PIT, and preserves interrupt frames.
; -----------------------------------------------------------------------------

; First protected-mode exception path. These handlers run in ring 0 with the
; flat kernel segments already loaded by protected_mode_entry.

BITS 32

KERNEL_CODE_SELECTOR equ 0x08
IDT_ENTRY_COUNT      equ 256
EXCEPTION_COUNT      equ 32
IRQ0_VECTOR          equ 0x20
YIELD_VECTOR         equ 0x30
PIC_MASTER_COMMAND   equ 0x20
PIC_MASTER_DATA      equ 0x21
PIC_SLAVE_COMMAND    equ 0xA0
PIC_SLAVE_DATA       equ 0xA1
PIT_CHANNEL0_DATA    equ 0x40
PIT_COMMAND          equ 0x43
; The PIT's approximately 1,193,182 Hz input divided by 11,932 yields
; approximately 100 Hz. Round to the nearest representable divisor.
PIT_CHANNEL0_DIVISOR equ 11932

extern _exception_handler
extern _timer_handler
extern _scheduler_select
global _interrupts_init
global _interrupts_enable_and_halt

section .text

; Exceptions without a processor-pushed error code get a synthetic zero.
%macro ISR_NO_ERROR 1
; All exceptions must present the same vector/error layout to C. A synthetic
; zero fills the CPU's missing error-code slot; it is removed before IRETD.
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

; IRQ0 has no CPU-pushed error code. Preserve every general register because
; this interrupt can arrive between any two instructions in the interrupted code.
irq0_stub:
    pushad
    cld

    ; Align the stack for the existing 32-bit C ABI before calling the handler.
    mov ebp, esp
    ; Clear the low four bits to round down to a 16-byte C call-site boundary.
    ; EBP remembers the untouched frame so alignment padding is reversible.
    and esp, 0xFFFFFFF0
    call _timer_handler
    ; Select while IF remains clear. The returned frame may be on another
    ; thread's stack; keep it in EBP until the PIC has been acknowledged.
    sub esp, 12
    push ebp
    call _scheduler_select
    mov ebp, eax

    ; IRQ0 is on the master PIC, so acknowledge it through the master's command
    ; port (0x20) with the non-specific EOI command (0x20).
    mov al, 0x20
    out PIC_MASTER_COMMAND, al

    mov esp, ebp
    popad
    iretd                      ; Restores EIP, CS, and the interrupted EFLAGS/IF.

; Software yield presents exactly the same PUSHAD/IRETD frame as IRQ0, but
; is not a PIC interrupt and must not send an end-of-interrupt command.
yield_stub:
    pushad
    cld
    mov ebp, esp
    and esp, 0xFFFFFFF0
    sub esp, 12
    push ebp
    call _scheduler_select
    mov esp, eax
    popad
    iretd

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
    ; Each IDT gate splits a 32-bit entry address into two 16-bit halves.
    ; The selector chooses the flat code segment, not the handler address.
    mov [edi], ax              ; Handler offset bits 0..15.
    mov word [edi + 2], KERNEL_CODE_SELECTOR
    mov byte [edi + 4], 0      ; Reserved byte.
    mov byte [edi + 5], 0x8E   ; P=1, DPL=0, 32-bit interrupt gate.
    shr eax, 16
    mov [edi + 6], ax          ; Handler offset bits 16..31.
    add edi, 8
    loop .install_exception_gate

    ; Keep hardware IRQ0 in this same IDT, immediately after CPU exceptions.
    mov eax, irq0_stub
    mov edi, idt + IRQ0_VECTOR * 8
    mov [edi], ax
    mov word [edi + 2], KERNEL_CODE_SELECTOR
    mov byte [edi + 4], 0
    mov byte [edi + 5], 0x8E
    shr eax, 16
    mov [edi + 6], ax

    ; Ring-0 software interrupt used for explicit cooperative switching.
    mov eax, yield_stub
    mov edi, idt + YIELD_VECTOR * 8
    mov [edi], ax
    mov word [edi + 2], KERNEL_CODE_SELECTOR
    mov byte [edi + 4], 0
    mov byte [edi + 5], 0x8E
    shr eax, 16
    mov [edi + 6], ax

    lidt [idtr]

    ; ICW1: initialize both 8259s, expect ICW4. Command ports 0x20/0xA0
    ; control the master/slave PICs; data ports 0x21/0xA1 take their settings.
    mov al, 0x11
    out PIC_MASTER_COMMAND, al
    call pic_io_wait
    out PIC_SLAVE_COMMAND, al
    call pic_io_wait

    ; ICW2: move master IRQs to 0x20..0x27 and slave IRQs to 0x28..0x2F.
    mov al, 0x20
    out PIC_MASTER_DATA, al
    call pic_io_wait
    mov al, 0x28
    out PIC_SLAVE_DATA, al
    call pic_io_wait

    ; ICW3: the slave is attached to master IRQ2; the slave's ID is 2.
    mov al, 0x04
    out PIC_MASTER_DATA, al
    call pic_io_wait
    mov al, 0x02
    out PIC_SLAVE_DATA, al
    call pic_io_wait

    ; ICW4: use 8086 mode on both PICs.
    mov al, 0x01
    out PIC_MASTER_DATA, al
    call pic_io_wait
    out PIC_SLAVE_DATA, al
    call pic_io_wait

    ; Channel 0, low byte then high byte, mode 2 rate generator, binary count.
    ; Program the complete divisor while CPU interrupts are still disabled.
    mov al, 0x34
    out PIT_COMMAND, al
    mov ax, PIT_CHANNEL0_DIVISOR
    out PIT_CHANNEL0_DATA, al
    mov al, ah
    out PIT_CHANNEL0_DATA, al

    ; Mask every line except master IRQ0. IRQ2 remains masked, so no slave
    ; interrupt can arrive while the slave PIC is fully masked as well.
    mov al, 0xFE
    out PIC_MASTER_DATA, al
    mov al, 0xFF
    out PIC_SLAVE_DATA, al

    popad
    ret

; Common legacy delay between PIC command/data writes (port 0x80 is unused).
pic_io_wait:
    ; A harmless legacy port write gives the PIC time to process commands.
    out 0x80, al
    ret

; Enable maskable interrupts only when the caller is ready to idle. STI delays
; recognition through the following HLT, so a pending IRQ cannot be missed.
_interrupts_enable_and_halt:
    sti
    hlt
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
