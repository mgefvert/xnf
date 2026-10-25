; -----------------------------------------------------------------------------
; BIOS boot sector: load the adjacent flat kernel image and transfer control.
; Uses one INT 13h extended read into physical 0x10000; remains exactly 512 bytes.
; -----------------------------------------------------------------------------

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
    ; Keep the boot stack below the sector: pushes grow toward lower addresses
    ; and must not overwrite the instructions the BIOS placed at 0x7C00.
    mov sp, 0x7C00
    mov [boot_drive], dl       ; BIOS passes the selected boot drive in DL.
    sti

    ; Check that INT 13h Extensions and extended reads are supported.
    mov bx, 0x55AA
    mov ah, 0x41
    mov dl, [boot_drive]
    int 0x13
    jc disk_error
    cmp bx, 0xAA55             ; Swapped signature proves extensions responded.
    jne disk_error
    test cx, 1                ; Capability bit 0 advertises extended disk access.
    jz disk_error

    ; DS:SI points to the DAP; DL selects the BIOS boot drive.
    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jc disk_error

    ; Real-mode physical address = segment * 16 + offset. A far jump sets CS
    ; as well as IP, so kernel entry sees physical 0x10000 as CS:0000.
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
    ; Disk Address Packet for AH=42h. Buffer order is offset, then segment;
    ; LBA 1 starts immediately after this boot sector's LBA 0.
    db 0x10
    db 0
    dw KERNEL_SECTORS
    dw 0x0000
    dw 0x1000
    dq 1

; $-$$ is the emitted sector size so far. The BIOS requires the final two
; bytes to be 55 AA; NASM's little-endian word emits them from 0xAA55.
times 510 - ($ - $$) db 0
dw 0xAA55
