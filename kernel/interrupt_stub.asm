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
    %if i == 8
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

section .data
align 8
global isr_stub_table
isr_stub_table:
%assign i 0
%rep 256
    dq isr%+i   
%assign i i+1
%endrep