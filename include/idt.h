#ifndef IDT_H
#define IDT_H

#include <stdint.h>
#include "trap.h"

void handle_syscall(Trapframe *tf);

/* syscall 指令入口（在 interrupt_stub.asm 中） */
extern void syscall_entry(void);

/* syscall 内核栈和用户 RSP 保存区（在 syscall.c 中） */
extern uint64_t g_syscall_kernel_stack;
extern uint64_t g_saved_user_rsp;

/* 用户程序退出标志 – 在 syscall_entry 汇编中检查（在 syscall.c 中） */
extern int g_exit_program;

// ----------------- trap常量 -----------------

#define T_DIVIDE     0
#define T_DEBUG      1
#define T_NMI        2
#define T_BRKPT      3
#define T_OFLOW      4
#define T_BOUND      5
#define T_ILLOP      6
#define T_DEVICE     7
#define T_DBLFLT     8
#define T_TSS       10
#define T_SEGNP     11
#define T_STACK     12
#define T_GPFLT     13
#define T_PGFLT     14
#define T_FPERR     16
#define T_ALIGN     17
#define T_MCHK      18
#define T_SIMDERR   19

#define T_SYSCALL   48
#define T_DEFAULT   500

#define IRQ_OFFSET  32

#define IRQ_TIMER        0
#define IRQ_KBD          1
#define IRQ_SERIAL       4
#define IRQ_SPURIOUS     7
#define IRQ_IDE         14
#define IRQ_ERROR       19

// ----------------- idt描述符 -----------------

struct idt_entry {
    uint16_t isr_low;
    uint16_t kernel_cs;
    uint8_t  ist;
    uint8_t  attributes;
    uint16_t isr_mid;
    uint32_t isr_high;
    uint32_t reserved;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/*typedef struct Trapframe {
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t rbp;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;

    uint64_t int_no;
    uint64_t error_code;

    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} __attribute__((packed)) Trapframe;
*/

typedef Trapframe interrupt_frame_t;

typedef void (*interrupt_handler_t)(Trapframe* tf);

void idt_init(void);
void register_interrupt_handler(uint8_t n, interrupt_handler_t handler);
void send_eoi(int int_no);

#endif 
