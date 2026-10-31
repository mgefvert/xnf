; -----------------------------------------------------------------------------
; First entry to a software-switched kernel thread and explicit yield.
; The scheduler supplies an IRETD frame; C owns thread policy and lifetime.
; -----------------------------------------------------------------------------

BITS 32
extern _scheduler_run_current
global _scheduler_thread_entry
global _scheduler_yield

section .text

; IRETD enters with ESP at the top of the new stack, not at a cdecl call
; frame. Align the call site explicitly and never return to nonexistent code.
_scheduler_thread_entry:
    cld
    and esp, 0xFFFFFFF0
    call _scheduler_run_current
.halt:
    cli
    hlt
    jmp .halt

; A ring-0 interrupt gate saves EFLAGS and disables IRQs while selecting.
; IRETD restores the caller's IF and general registers on resumption.
_scheduler_yield:
    int 0x30
    ret
