; -----------------------------------------------------------------------------
; Activate the already prepared 32-bit x86 paging structures.
; The transition preserves caller interrupt state and allocates no memory.
; -----------------------------------------------------------------------------

BITS 32
section .text

global _paging_activate

; void paging_activate(uint32_t directory_physical), 32-bit cdecl.
; Flat protected-mode segments are already loaded. paging_prepare identity-maps
; this code, the caller/return address, and the entire linker-reserved stack.
_paging_activate:
    pushfd
    cli

    ; The prepared directory uses non-PAE, 4 KiB entries. Preserve other CR4 bits.
    mov eax, cr4
    and eax, 0xFFFFFFCF        ; Clear PAE (bit 5) and PSE (bit 4).
    mov cr4, eax

    ; cdecl entry stack: [ESP] = return address, [ESP+4] = directory address.
    ; PUSHFD added four bytes, so the argument is now eight bytes above ESP.
    mov eax, [esp + 8]        ; Argument follows saved EFLAGS and return address.
    mov cr3, eax              ; CR3 takes a physical base, never a virtual pointer.
    mov eax, cr0
    or eax, 0x80010000        ; PG and WP: supervisor writes honor read-only PTEs.
    ; Code and stack stay at the same linear addresses because the prepared
    ; bootstrap map is identity-mapped. The return path is already present.
    mov cr0, eax
    jmp short .enabled       ; Continue fetching from the identity-mapped code.
.enabled:
    popfd                    ; Restore the caller's IF only after activation.
    ret
