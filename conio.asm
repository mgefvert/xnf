; 32-bit protected-mode text console for XNF.
; All public functions use cdecl: arguments are on the stack, the caller
; removes arguments, and EAX/ECX/EDX are caller-saved. EBX/ESI/EDI/EBP are
; preserved by functions that use them.

BITS 32

CONIO_SCREEN_WIDTH equ 80
CONIO_SCREEN_HEIGHT equ 25
CONIO_SCREEN_CELLS equ CONIO_SCREEN_WIDTH * CONIO_SCREEN_HEIGHT
CONIO_VIDEO_MEMORY equ 0x000B8000

; void clear_screen(void)
; Clear all cells using the current attribute and reset the software cursor.
clear_screen:
    push edi
    cld
    mov edi, CONIO_VIDEO_MEMORY
    mov ecx, CONIO_SCREEN_CELLS
    mov al, ' '
    mov ah, [KERNEL_LOAD_ADDRESS + conio_color]
    rep stosw
    mov word [KERNEL_LOAD_ADDRESS + conio_cursor], 0
    call conio_sync_hw_cursor
    pop edi
    ret

; void kputc(uint32_t character)
; Newline advances one row, carriage return returns to column zero, and
; backspace erases the previous cell. Other bytes are written directly.
kputc:
    push ebp
    mov ebp, esp
    push ebx
    push esi
    push edi
    cld

    mov al, [ebp + 8]
    cmp al, 10
    je .newline
    cmp al, 13
    je .carriage_return
    cmp al, 8
    je .backspace

    movzx edi, word [KERNEL_LOAD_ADDRESS + conio_cursor]
    lea edi, [edi * 2 + CONIO_VIDEO_MEMORY]
    mov ah, [KERNEL_LOAD_ADDRESS + conio_color]
    mov [edi], ax
    inc word [KERNEL_LOAD_ADDRESS + conio_cursor]
    cmp word [KERNEL_LOAD_ADDRESS + conio_cursor], CONIO_SCREEN_CELLS
    jb .done
    call .scroll
    jmp .done

.newline:
    movzx eax, word [KERNEL_LOAD_ADDRESS + conio_cursor]
    xor edx, edx
    mov ebx, CONIO_SCREEN_WIDTH
    div ebx
    inc eax
    imul eax, eax, CONIO_SCREEN_WIDTH
    mov [KERNEL_LOAD_ADDRESS + conio_cursor], ax
    cmp ax, CONIO_SCREEN_CELLS
    jb .done
    call .scroll
    jmp .done

.carriage_return:
    movzx eax, word [KERNEL_LOAD_ADDRESS + conio_cursor]
    xor edx, edx
    mov ebx, CONIO_SCREEN_WIDTH
    div ebx
    imul eax, ebx
    mov [KERNEL_LOAD_ADDRESS + conio_cursor], ax
    jmp .done

.backspace:
    cmp word [KERNEL_LOAD_ADDRESS + conio_cursor], 0
    je .done
    dec word [KERNEL_LOAD_ADDRESS + conio_cursor]
    movzx edi, word [KERNEL_LOAD_ADDRESS + conio_cursor]
    lea edi, [edi * 2 + CONIO_VIDEO_MEMORY]
    mov al, ' '
    mov ah, [KERNEL_LOAD_ADDRESS + conio_color]
    mov [edi], ax
    jmp .done

.scroll:
    ; Move rows 1..24 up, clear the last row, and leave the cursor there.
    cld
    mov esi, CONIO_VIDEO_MEMORY + CONIO_SCREEN_WIDTH * 2
    mov edi, CONIO_VIDEO_MEMORY
    mov ecx, CONIO_SCREEN_WIDTH * (CONIO_SCREEN_HEIGHT - 1)
    rep movsw

    mov edi, CONIO_VIDEO_MEMORY + CONIO_SCREEN_WIDTH * (CONIO_SCREEN_HEIGHT - 1) * 2
    mov ecx, CONIO_SCREEN_WIDTH
    mov al, ' '
    mov ah, [KERNEL_LOAD_ADDRESS + conio_color]
    rep stosw
    mov word [KERNEL_LOAD_ADDRESS + conio_cursor], CONIO_SCREEN_WIDTH * (CONIO_SCREEN_HEIGHT - 1)
    ret

.done:
    call conio_sync_hw_cursor
    pop edi
    pop esi
    pop ebx
    mov esp, ebp
    pop ebp
    ret

; void kputs(const char *string)
kputs:
    push ebp
    mov ebp, esp
    push esi
    cld
    mov esi, [ebp + 8]
.next_character:
    lodsb
    test al, al
    jz .done
    movzx eax, al
    push eax
    call kputc
    add esp, 4
    jmp .next_character
.done:
    pop esi
    pop ebp
    ret

; void kprint_hex(uint32_t value)
; Print a fixed-width, uppercase hexadecimal value with a 0x prefix.
kprint_hex:
    push ebp
    mov ebp, esp
    push ebx
    push esi
    mov ebx, [ebp + 8]

    push dword '0'
    call kputc
    add esp, 4
    push dword 'x'
    call kputc
    add esp, 4

    mov esi, 8
.next_digit:
    mov eax, ebx
    shr eax, 28
    and eax, 0x0F
    cmp eax, 9
    jbe .numeric_digit
    add eax, 'A' - 10
    jmp .emit_digit
.numeric_digit:
    add eax, '0'
.emit_digit:
    push eax
    call kputc
    add esp, 4
    shl ebx, 4
    dec esi
    jnz .next_digit

    pop esi
    pop ebx
    pop ebp
    ret

; void kprint_dec(uint32_t value)
kprint_dec:
    push ebp
    mov ebp, esp
    sub esp, 12                 ; Enough local space for ten decimal digits.
    push ebx
    push esi
    push edi

    mov eax, [ebp + 8]
    mov ebx, 10
    lea edi, [ebp - 1]
    xor ecx, ecx
.make_digit:
    xor edx, edx
    div ebx
    add dl, '0'
    mov [edi], dl
    dec edi
    inc ecx
    test eax, eax
    jnz .make_digit

    lea esi, [edi + 1]
    mov edi, ecx
.emit_digit:
    movzx eax, byte [esi]
    push eax
    call kputc
    add esp, 4
    inc esi
    dec edi
    jnz .emit_digit

    pop edi
    pop esi
    pop ebx
    mov esp, ebp
    pop ebp
    ret

; void ksetcolor(uint32_t attribute)
ksetcolor:
    push ebp
    mov ebp, esp
    mov eax, [ebp + 8]
    mov [KERNEL_LOAD_ADDRESS + conio_color], al
    pop ebp
    ret

; uint32_t kgetcolor(void)
kgetcolor:
    movzx eax, byte [KERNEL_LOAD_ADDRESS + conio_color]
    ret

; void ksetcursor(uint32_t column, uint32_t row)
; Coordinates beyond the screen are clamped to its last column or row.
ksetcursor:
    push ebp
    mov ebp, esp
    mov eax, [ebp + 8]
    cmp eax, CONIO_SCREEN_WIDTH - 1
    jbe .column_valid
    mov eax, CONIO_SCREEN_WIDTH - 1
.column_valid:
    mov edx, [ebp + 12]
    cmp edx, CONIO_SCREEN_HEIGHT - 1
    jbe .row_valid
    mov edx, CONIO_SCREEN_HEIGHT - 1
.row_valid:
    imul edx, edx, CONIO_SCREEN_WIDTH
    add eax, edx
    mov [KERNEL_LOAD_ADDRESS + conio_cursor], ax
    call conio_sync_hw_cursor
    pop ebp
    ret

; void kgetcursor(uint32_t *column, uint32_t *row)
; Both output pointers must refer to writable 32-bit values.
kgetcursor:
    push ebp
    mov ebp, esp
    movzx eax, word [KERNEL_LOAD_ADDRESS + conio_cursor]
    xor edx, edx
    mov ecx, CONIO_SCREEN_WIDTH
    div ecx
    mov ecx, [ebp + 8]
    mov [ecx], edx
    mov ecx, [ebp + 12]
    mov [ecx], eax
    pop ebp
    ret

; Copy the software cell index into the VGA color CRT controller cursor
; registers. This kernel uses color text memory at 0xB8000, so the CRTC ports
; are 0x3D4 (index) and 0x3D5 (data).
conio_sync_hw_cursor:
    push eax
    push edx

    mov dx, 0x3D4
    mov al, 0x0A               ; Cursor start scan line; bit 5 hides the cursor.
    out dx, al
    inc dx
    in al, dx
    and al, 0xDF               ; Preserve shape and make the cursor visible.
    out dx, al

    dec dx                     ; Return to the CRTC index port.
    mov al, 0x0F               ; Cursor location low byte.
    out dx, al
    inc dx
    mov al, [KERNEL_LOAD_ADDRESS + conio_cursor]
    out dx, al

    dec dx                     ; Return to the CRTC index port.
    mov al, 0x0E               ; Cursor location high byte.
    out dx, al
    inc dx
    mov al, [KERNEL_LOAD_ADDRESS + conio_cursor + 1]
    out dx, al

    pop edx
    pop eax
    ret

align 4, db 0
conio_color:
    db 0x07
conio_cursor:
    dw 0
