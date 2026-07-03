[bits 64]

; 在用户态运行的测试代码(编译链接进内核,但会被复制到用户页执行)
; 此代码会在ring 3下通过int 48 (T_SYSCALL)调用syscall
global usermode_test_start
global usermode_test_end

section .usertest progbits alloc exec write

usermode_test_start:
    ; print "hello from ring 3!\n" via sys_write
    lea rdi, [rel msg]
    mov rsi, 20         ; len
    mov rax, 0          ; sys_write
    mov rdx, 1          ; fd=stdout
    ; sysv calling convention: rdi=msg, rsi=len, rdx=fd
    ; for int 0x80: rax=nr, rdi=arg1(fd), rsi=arg2(buf), rdx=arg3(len)
    ; but int 0x80 uses different arg mapping than syscall instruction
    ; fix: rax=nr, rbx=arg1, rcx=arg2, rdx=arg3 (老的int 0x80约定)

    ; Let's use a clean approach:
    ; rax = syscall number (0 = SYS_WRITE)
    ; rdi = fd (1)
    ; rsi = buf
    ; rdx = len
    ; r10 = (unused for SYS_WRITE)
    mov rdi, 1
    lea rsi, [rel msg]
    mov rdx, 20
    mov rax, 0
    int 48

    ; print "exiting...\n" via sys_write
    mov rdi, 1
    lea rsi, [rel msg2]
    mov rdx, 13
    mov rax, 0
    int 48

    ; sys_exit
    mov rax, 1
    xor rdi, rdi
    int 48

    ; 不应该到达这里
    jmp $

msg: db "hello from ring 3!", 10, 0   ; "hello from ring 3!\n\0"
msg2: db "exiting...", 10, 0

usermode_test_end:
