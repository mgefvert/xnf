BITS 16
ORG 0x7C00

%include "kernel_sectors.inc"

start:
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    mov [boot_drive], dl
    sti

    ; Check that INT 13h Extensions and extended reads are supported.
    mov bx, 0x55AA
    mov ah, 0x41
    mov dl, [boot_drive]
    int 0x13
    jc disk_error
    cmp bx, 0xAA55
    jne disk_error
    test cx, 1
    jz disk_error

    ; DS:SI points to the DAP; DL selects the BIOS boot drive.
    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jc disk_error

    jmp 0x1000:0x0000

disk_error:
    cli
    mov ax, 0xB800
    mov es, ax
    mov word [es:0], 0x0C45 ; Red "E" for disk error.
.halt:
    hlt
    jmp .halt

boot_drive db 0

align 4, db 0
dap:
    db 0x10
    db 0
    dw KERNEL_SECTORS
    dw 0x0000
    dw 0x1000
    dq 1

times 510 - ($ - $$) db 0
dw 0xAA55
