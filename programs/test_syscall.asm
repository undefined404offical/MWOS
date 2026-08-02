; ============================
; MWOS Syscall Test Program
; Tests: write, brk, exit
; Uses syscall instruction (not int 48)
; ============================

; Define syscall numbers (from syscall_nr.h)
SYS_write equ 1
SYS_brk   equ 12
SYS_exit  equ 60

global _start

section .text

_start:
    ; Test 1: write to stdout
    mov rax, SYS_write
    mov rdi, 1          ; fd = stdout
    lea rsi, [rel msg]
    mov rdx, msg_len
    syscall

    ; Test 2: brk - get current brk
    mov rax, SYS_brk
    xor rdi, rdi        ; addr = 0 (get current)
    syscall
    mov rbx, rax        ; save original brk in rbx

    ; Test 3: brk - extend heap
    mov rax, SYS_brk
    lea rdi, [rbx + 4096]  ; brk + 4096
    syscall
    cmp rax, rbx
    jle brk_fail

    ; Write success message
    mov rax, SYS_write
    mov rdi, 1
    lea rsi, [rel ok_msg]
    mov rdx, ok_msg_len
    syscall

    ; Exit with code 0
    mov rax, SYS_exit
    xor rdi, rdi        ; exit code 0
    syscall

brk_fail:
    ; Write failure message
    mov rax, SYS_write
    mov rdi, 1
    lea rsi, [rel fail_msg]
    mov rdx, fail_msg_len
    syscall

    ; Exit with code 1
    mov rax, SYS_exit
    mov rdi, 1
    syscall

section .data
msg:     db 'Hello from MWOS user program!', 0x0a
msg_len  equ $ - msg
ok_msg:  db 'brk test passed!', 0x0a
ok_msg_len equ $ - ok_msg
fail_msg: db 'brk test FAILED!', 0x0a
fail_msg_len equ $ - fail_msg