BITS 16
section .text

KERNEL_LOAD_ADDRESS equ 0x10000

%define RM_OFFSET(label) (label - start)
%define KERNEL_ADDRESS(label) (KERNEL_LOAD_ADDRESS + label - start)

NULL_SELECTOR        equ 0x00
KERNEL_CODE_SELECTOR equ 0x08
KERNEL_DATA_SELECTOR equ 0x10

E820_FUNCTION    equ 0xE820
E820_SIGNATURE   equ 0x534D4150
E820_ENTRY_SIZE  equ 24
E820_MAX_ENTRIES equ 32
E820_MAP_SIZE    equ E820_ENTRY_SIZE * E820_MAX_ENTRIES

extern _kernel_main             ; MinGW i386 COFF prefixes C names with '_'.
extern _boot_memory_check
extern __bss_start
extern __bss_end
extern __kernel_stack_top       ; Linker-defined physical address in the flat segment.

start:
    cli
    cld

    ; In real mode, kernel labels are offsets within CS=DS=SS=0x1000.
    mov ax, cs
    mov ds, ax
    mov es, ax
    mov ss, ax
    ; The temporary stack is part of the BIOS-loaded image, not unvalidated
    ; .bss or the final stack. It also serves the first protected-mode C call.
    mov eax, boot_stack_top
    sub eax, KERNEL_LOAD_ADDRESS
    mov sp, ax

    ; Require i686 (Pentium Pro or later), matching C's -march=i686. Test for
    ; CPUID before using it; CR4 and supervisor write protection are required.
    pushfd
    pop ecx
    mov eax, ecx
    xor eax, 1 << 21
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    xor eax, ecx
    test eax, 1 << 21
    jz unsupported_cpu
    mov eax, 1
    cpuid
    shr eax, 8
    and eax, 0xF
    cmp eax, 6
    jb unsupported_cpu

    ; BIOS E820 is only available before entering protected mode.
    ; CF is set if the map could not be collected completely.
    call memory_detect
    setc byte [RM_OFFSET(_e820_detection_failed)]

    ; Enable A20 through the Fast A20 Gate (port 0x92). Keep reset bit 0 clear.
    in al, 0x92
    or al, 00000010b
    and al, 11111110b
    out 0x92, al

    ; Flat ring-0 GDT: null, 32-bit code, and 32-bit data descriptors.
    lgdt [RM_OFFSET(gdtr)]

    mov eax, cr0
    or eax, 1                  ; CR0.PE = 1: enter protected mode.
    mov cr0, eax

    ; BITS 16 needs an operand-size override so this far pointer has a 32-bit offset.
    jmp dword KERNEL_CODE_SELECTOR:protected_mode_entry_address

unsupported_cpu:
    mov si, RM_OFFSET(cpu_message)
.print:
    lodsb
    test al, al
    jz .halt
    mov ah, 0x0E
    mov bx, 7
    int 0x10
    jmp .print
.halt:
    cli
    hlt
    jmp .halt
cpu_message: db "Unsupported CPU: i686 required", 0

; Collect the BIOS E820 map into e820_map.
;
; Input assumptions:
;   - Called in 16-bit real mode on an 80386 or later.
;   - CS addresses this kernel at physical 0x10000; the map is addressed as
;     CS:offset, so its ES:DI physical address remains below 1 MiB.
;   - The BIOS supports INT 15h, EAX=E820h.
;
; Return:
;   - CF clear on success, CF set on a BIOS or buffer-capacity error.
;   - [e820_entry_count] contains the number of returned, valid entries.
;
; Entry layout (24 bytes):
;   offset  size  field
;      0      8   uint64 base_address
;      8      8   uint64 length
;     16      4   uint32 type
;     20      4   uint32 attributes
;
; The attribute field is initialized to 1 before each call as required by the
; 24-byte E820 interface. Legacy 20-byte entries are normalized to enabled
; (attributes = 1), so import can distinguish them from disabled extended entries.
memory_detect:
    pushad
    push ds
    push es

    cld
    push cs
    pop ds
    push cs
    pop es

    ; Clear all slots; unused slots remain zero.
    xor ax, ax
    mov di, RM_OFFSET(_e820_map)
    mov cx, E820_MAP_SIZE / 2
    rep stosw

    mov word [RM_OFFSET(_e820_entry_count)], 0
    mov dword [RM_OFFSET(_e820_continuation)], 0

.next_entry:
    ; If a prior response supplied another continuation value, the map is full.
    cmp word [RM_OFFSET(_e820_entry_count)], E820_MAX_ENTRIES
    jae .failure

    ; ES:DI points at the next 24-byte slot. Entry count is at most 31 here.
    mov ax, [RM_OFFSET(_e820_entry_count)]
    mov bx, E820_ENTRY_SIZE
    mul bx
    add ax, RM_OFFSET(_e820_map)
    mov di, ax
    mov [RM_OFFSET(_e820_slot_offset)], di

    ; E820 callers set attributes bit 0 to request the extended attributes.
    mov dword [es:di + 20], 1

    mov eax, E820_FUNCTION
    mov edx, E820_SIGNATURE
    mov ecx, E820_ENTRY_SIZE
    mov ebx, [RM_OFFSET(_e820_continuation)]

    ; Keep the kernel data segments valid even if the BIOS changes DS or ES.
    ; ES:DI remains set to the destination for the duration of INT 15h.
    push ds
    push es
    int 0x15
    pop es
    pop ds
    mov di, [RM_OFFSET(_e820_slot_offset)]

    jc .failure
    cmp eax, E820_SIGNATURE
    jne .failure

    ; BIOSes return either a 20-byte legacy entry or the requested 24 bytes.
global memory_detect.response_size
.response_size:
    cmp ecx, 20
    je .legacy_entry
    cmp ecx, E820_ENTRY_SIZE
    jne .failure
    jmp .entry_ready

.legacy_entry:
    mov dword [es:di + 20], 1

.entry_ready:
    mov [RM_OFFSET(_e820_continuation)], ebx
    inc word [RM_OFFSET(_e820_entry_count)]
    test ebx, ebx
    jnz .next_entry

    pop es
    pop ds
    popad
    clc
    ret

.failure:
    pop es
    pop ds
    popad
    stc
    ret

; E820 entries retain BIOS order; legacy attributes are normalized to enabled.
align 4, db 0
global _e820_entry_count
global _e820_detection_failed

_e820_entry_count:      dw 0
_e820_continuation:     dd 0
_e820_slot_offset:      dw 0
_e820_detection_failed: db 0

align 4, db 0
global _e820_map

_e820_map:              times E820_MAP_SIZE db 0

align 8, db 0
gdt_start:

gdt_null:
    dq 0                       ; Selector 0x00: mandatory null descriptor.
gdt_code:
    dw 0xFFFF                  ; Limit bits 0..15.
    dw 0x0000                  ; Base bits 0..15.
    db 0x00                    ; Base bits 16..23.
    db 10011010b               ; Present, ring 0, code, executable/readable.
    db 11001111b               ; 4 KiB granularity, 32-bit, limit bits 16..19.
    db 0x00                    ; Base bits 24..31.
gdt_data:
    dw 0xFFFF                  ; Limit bits 0..15.
    dw 0x0000                  ; Base bits 0..15.
    db 0x00                    ; Base bits 16..23.
    db 10010010b               ; Present, ring 0, data, writable.
    db 11001111b               ; 4 KiB granularity, 32-bit, limit bits 16..19.
    db 0x00                    ; Base bits 24..31.
gdt_end:

gdtr:
    dw gdt_end - gdt_start - 1 ; GDT size in bytes, minus one.
    dd KERNEL_ADDRESS(gdt_start) ; Linker places the flat image at 0x10000.

protected_mode_entry_address equ KERNEL_ADDRESS(protected_mode_entry)

BITS 32
protected_mode_entry:
    mov ax, KERNEL_DATA_SELECTOR
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Validate the complete destination extent BEFORE touching .bss or using
    ; the final stack. The temporary stack lives in the loaded image.
    mov esp, boot_stack_top
    call _boot_memory_check
    mov esp, __kernel_stack_top
    cld

    ; There is no C runtime to clear zero-initialized data for us.
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    xor eax, eax
    rep stosb

    call _interrupts_init
    call _kernel_main

.halt:
    cli
    hlt
    jmp .halt

%include "paging.asm"
%include "interrupts.asm"

section .data
align 16, db 0
boot_stack: times 2048 db 0
boot_stack_top:
