[bits 64]
global enter_usermode

; ============================================================
; enter_usermode(uint64_t entry, uint64_t stack)
; 通过iretq进入ring 3
; rdi -> entry (user function rip)
; rsi -> stack top (user rsp)
; ============================================================
enter_usermode:
    ; set up user data segments
    mov ax, 0x23        ; SEL_UDATA
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; build iretq frame (bottom to top: rip, cs, rflags, rsp, ss)
    push 0x23           ; ss = SEL_UDATA
    push rsi            ; rsp = user stack
    push 0x202          ; rflags (IF=1)
    push 0x1B           ; cs = SEL_UCODE
    push rdi            ; rip = entry
    iretq
    ; never returns here
