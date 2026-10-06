BITS 16
ORG 0x0000

KERNEL_LOAD_ADDRESS equ 0x10000
PROTECTED_STACK_TOP equ 0x90000

NULL_SELECTOR equ 0x00
KERNEL_CODE_SELECTOR equ 0x08
KERNEL_DATA_SELECTOR equ 0x10

E820_FUNCTION equ 0xE820
E820_SIGNATURE equ 0x534D4150
E820_ENTRY_SIZE equ 24
E820_MAX_ENTRIES equ 32
E820_MAP_SIZE equ E820_ENTRY_SIZE * E820_MAX_ENTRIES

start:
    cli
    cld

    ; In real mode, kernel labels are offsets within CS=DS=SS=0x1000.
    mov ax, cs
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0xFFFE

    ; BIOS E820 is only available before entering protected mode.
    ; CF is set if the map could not be collected completely.
    call memory_detect

    ; Enable A20 through the Fast A20 Gate (port 0x92). Keep reset bit 0 clear.
    in al, 0x92
    or al, 00000010b
    and al, 11111110b
    out 0x92, al

    ; Flat ring-0 GDT: null, 32-bit code, and 32-bit data descriptors.
    lgdt [gdtr]

    mov eax, cr0
    or eax, 1                  ; CR0.PE = 1: enter protected mode.
    mov cr0, eax

    ; BITS 16 needs an operand-size override so this far pointer has a 32-bit offset.
    jmp dword KERNEL_CODE_SELECTOR:protected_mode_entry_address

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
; 24-byte E820 interface. If the BIOS reports a legacy 20-byte entry, that
; field is reset to zero. The full map is cleared before collection.
memory_detect:
    pushad
    push ds
    push es

    cld
    push cs
    pop ds
    push cs
    pop es

    ; Clear all slots so legacy 20-byte entries have zero attributes and any
    ; unused slots remain zero as well.
    xor ax, ax
    mov di, e820_map
    mov cx, E820_MAP_SIZE / 2
    rep stosw

    mov word [e820_entry_count], 0
    mov dword [e820_continuation], 0

.next_entry:
    ; If a prior response supplied another continuation value, the map is full.
    cmp word [e820_entry_count], E820_MAX_ENTRIES
    jae .failure

    ; ES:DI points at the next 24-byte slot. Entry count is at most 31 here.
    mov ax, [e820_entry_count]
    mov bx, E820_ENTRY_SIZE
    mul bx
    add ax, e820_map
    mov di, ax
    mov [e820_slot_offset], di

    ; E820 callers set attributes bit 0 to request the extended attributes.
    mov dword [es:di + 20], 1

    mov eax, E820_FUNCTION
    mov edx, E820_SIGNATURE
    mov ecx, E820_ENTRY_SIZE
    mov ebx, [e820_continuation]

    ; Keep the kernel data segments valid even if the BIOS changes DS or ES.
    ; ES:DI remains set to the destination for the duration of INT 15h.
    push ds
    push es
    int 0x15
    pop es
    pop ds
    mov di, [e820_slot_offset]

    jc .failure
    cmp eax, E820_SIGNATURE
    jne .failure

    ; BIOSes return either a 20-byte legacy entry or the requested 24 bytes.
    cmp ecx, 20
    je .legacy_entry
    cmp ecx, E820_ENTRY_SIZE
    jne .failure
    jmp .entry_ready

.legacy_entry:
    mov dword [es:di + 20], 0

.entry_ready:
    mov [e820_continuation], ebx
    inc word [e820_entry_count]
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

; BIOS-reported E820 entries are kept verbatim in their original order.
align 4, db 0
e820_entry_count:
    dw 0
e820_continuation:
    dd 0
e820_slot_offset:
    dw 0
e820_map:
    times E820_MAP_SIZE db 0

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
    dd KERNEL_LOAD_ADDRESS + gdt_start ; Physical/linear base because the kernel is at 0x10000.

protected_mode_entry_address equ KERNEL_LOAD_ADDRESS + protected_mode_entry

BITS 32
protected_mode_entry:
    mov ax, KERNEL_DATA_SELECTOR
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Flat data segment, with a stack top at 0x90000 (below VGA/ROM, above the kernel).
    mov esp, PROTECTED_STACK_TOP
    cld

    call clear_screen

    mov esi,16
.again:
    mov eax, KERNEL_LOAD_ADDRESS + message
    push eax
    call kputs
    add esp, 4

    push esi
    call kprint_hex
    add esp, 4

    push dword ' '
    call kputc
    add esp, 4

    push esi
    call kprint_dec
    add esp, 4

    push dword 10
    call kputc
    add esp, 4

    dec esi
    jnz .again

.halt:
    cli
    hlt
    jmp .halt

message db 'xnf: ', 0

%include "conio.asm"
