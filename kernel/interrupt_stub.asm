[bits 64]
extern idt_handler
section .text

%macro ISR_NOERRCODE 1
global isr%1
isr%1:
    push qword 0     
    push qword %1    
    jmp isr_common
%endmacro

%macro ISR_ERRCODE 1
global isr%1
isr%1:
    push qword %1    
    jmp isr_common
%endmacro

%assign i 0
%rep 256
    ; x86_64 exceptions that push error codes:
    ;   8=DbleFault, 10=InvTSS, 11=SegNotPresent, 12=StackFault,
    ;   13=GPF, 14=PageFault, 17=AlignmentCheck
    %if i == 8 || i == 10 || i == 11 || i == 12 || i == 13 || i == 14 || i == 17
        ISR_ERRCODE i
    %else
        ISR_NOERRCODE i
    %endif
%assign i i+1
%endrep

isr_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    
    mov rdi, rsp 
    
    mov rbp, rsp
    and rsp, -16    
    
    call idt_handler
    
    mov rsp, rbp     
    
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    
    add rsp, 16 
    iretq             

; ============================================================
; Segment selectors (must match include/gdt.h)
; ============================================================
SEL_KDATA   equ 0x10
SEL_KCODE   equ 0x08
T_SYSCALL   equ 48

; ============================================================
; syscall_entry — Handle syscall instruction from user mode
; Registers on entry:
;   RAX = syscall number
;   RDI, RSI, RDX, R10, R8, R9 = arguments
;   RCX = return RIP (set by CPU)
;   R11 = RFLAGS (set by CPU)
; Returns:
;   RAX = syscall return value
; ============================================================
global syscall_entry
extern g_syscall_kernel_stack
extern g_saved_user_rsp
extern handle_syscall
extern g_exit_program

section .text
syscall_entry:
    ; 1. Save user RSP in global
    mov [g_saved_user_rsp], rsp
    
    ; 2. Switch to kernel stack
    mov rsp, [g_syscall_kernel_stack]
    
    ; 3. Build Trapframe on kernel stack (push order = reverse of struct)
    ; SS (kernel data segment)
    push qword SEL_KDATA
    ; User RSP (saved above)
    push qword [g_saved_user_rsp]
    ; RFLAGS (from R11, saved by syscall instruction)
    push r11
    ; CS (kernel code segment)
    push qword SEL_KCODE
    ; RIP (return address, from RCX, saved by syscall instruction)
    push rcx
    ; error_code = 0 (no error code for syscall)
    push qword 0
    ; int_no = T_SYSCALL
    push qword T_SYSCALL
    
    ; General-purpose registers
    push rax
    push rbx
    push rcx          ; RCX field (original RCX lost — contains return RIP)
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11          ; R11 field (original R11 lost — contains RFLAGS)
    push r12
    push r13
    push r14
    push r15
    
    ; 4. Call C handler
    mov rdi, rsp      ; arg1 = Trapframe*
    mov r12, rsp      ; save Trapframe pointer in r12
    
    ; Stack is already 16-byte aligned (22 * 8 = 176 = 11 * 16)
    cld
    call handle_syscall
    
    ; 5. Restore Trapframe pointer
    mov rsp, r12
    
    ; 5b. Check exit flag — if g_exit_program is set, use iretq to return to kernel
    cmp qword [g_exit_program], 0
    je .sysret_path
    
    ; === Exit path: use iretq to return to kernel mode ===
    ; Clear flag
    mov qword [g_exit_program], 0
    
    ; Read values from Trapframe
    mov rax, [rsp + 136]  ; tf->rip (elf_exit_handler)
    mov rbx, [rsp + 144]  ; tf->cs  (SEL_KCODE)
    mov rcx, [rsp + 152]  ; tf->rflags (0x202)
    mov rdx, [rsp + 160]  ; tf->rsp (kernel stack)
    mov rsi, [rsp + 168]  ; tf->ss  (SEL_KDATA)
    
    ; Restore RSP to kernel stack
    mov rsp, rdx
    
    ; Build iretq frame (same privilege level: only RIP, CS, RFLAGS)
    push rcx              ; RFLAGS
    push rbx              ; CS
    push rax              ; RIP
    iretq
    
.sysret_path:
    ; 6. Extract values needed for sysretq from Trapframe
    ; Offset map:
    ;   [rsp+112] = rax   (return value — set by handle_syscall)
    ;   [rsp+136] = rip   (return address)
    ;   [rsp+152] = rflags
    ;   [rsp+160] = rsp   (user stack)
    mov rax, [rsp + 112]  ; return value (tf->rax)
    mov rcx, [rsp + 136]  ; return address (tf->rip)
    mov r11, [rsp + 152]  ; RFLAGS (tf->rflags)
    
    ; 7. Restore user RSP and return to user mode
    mov rsp, [rsp + 160]  ; RSP = tf->rsp (user's original stack)
    
    ; Use o64 sysret to force 64-bit return (CS = STAR[63:48] + 16)
    o64 sysret

section .data
align 8
global isr_stub_table
isr_stub_table:
%assign i 0
%rep 256
    dq isr%+i   
%assign i i+1
%endrep