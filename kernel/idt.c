#include "idt.h"
#include "io.h"
#include "serial.h"
#include "trap.h"

extern void* isr_stub_table[];

__attribute__((aligned(0x10))) static struct idt_entry idt[256];
static struct idt_ptr idtr;

static interrupt_handler_t interrupt_handlers[256];

void register_interrupt_handler(uint8_t n, interrupt_handler_t handler) {
    interrupt_handlers[n] = handler;
}

static void idt_set_gate(uint8_t vector, void* isr, uint8_t flags) {
    uint64_t addr = (uint64_t)isr;

    idt[vector].isr_low = addr & 0xFFFF;
    idt[vector].kernel_cs = 0x08; // 内核代码段
    idt[vector].ist = 0;
    idt[vector].attributes = flags;
    idt[vector].isr_mid = (addr >> 16) & 0xFFFF;
    idt[vector].isr_high = (addr >> 32) & 0xFFFFFFFF;
    idt[vector].reserved = 0;
}

static void print_reg(const char* name, uint64_t val) {
    serial_puts(name);
    serial_puts(": ");
    serial_puthex64(val);
    serial_puts("  ");
}

void idt_handler(Trapframe* tf) {
    interrupt_handler_t handler = interrupt_handlers[tf->int_no];

    if (handler) {
        handler(tf);
    } else {
        if (tf->int_no < 32) {
            serial_puts("\n================ EXCEPTION DUMP ================\n");
            serial_puts("EXCEPTION: ");
            serial_putdec64(tf->int_no);

            if (tf->int_no == T_PGFLT) {
                uint64_t cr2;
                asm volatile("mov %%cr2, %0" : "=r"(cr2));
                serial_puts(" (PAGE FAULT)");
                serial_puts("\nFaulting Address (CR2): 0x");
                serial_puthex64(cr2);
            }

            serial_puts("\nError Code: ");
            serial_puthex64(tf->error_code);
            serial_puts("\n\n--- General Purpose Registers ---\n");

            print_reg("RAX", tf->rax);
            print_reg("RBX", tf->rbx);
            print_reg("RCX", tf->rcx);
            serial_puts("\n");
            print_reg("RDX", tf->rdx);
            print_reg("RSI", tf->rsi);
            print_reg("RDI", tf->rdi);
            serial_puts("\n");
            print_reg("RBP", tf->rbp);
            print_reg("R8 ", tf->r8);
            print_reg("R9 ", tf->r9);
            serial_puts("\n");
            print_reg("R10", tf->r10);
            print_reg("R11", tf->r11);
            print_reg("R12", tf->r12);
            serial_puts("\n");
            print_reg("R13", tf->r13);
            print_reg("R14", tf->r14);
            print_reg("R15", tf->r15);

            serial_puts("\n\n--- CPU State ---\n");
            print_reg("RIP", tf->rip);
            print_reg("CS ", tf->cs);
            serial_puts("\n");
            print_reg("RFLAGS", tf->rflags);
            print_reg("RSP", tf->rsp);
            print_reg("SS ", tf->ss);

            serial_puts("\n================================================\n");
            while (1) {
                asm volatile("hlt");
            }
        }
    }

    send_eoi(tf->int_no);
}

void send_eoi(int int_no) {
    if (int_no >= IRQ_OFFSET && int_no < IRQ_OFFSET + 16) {
        if (int_no >= IRQ_OFFSET + 8) {
            outb(0xA0, 0x20);
        }
        outb(0x20, 0x20);
    }
}

void idt_init(void) {
    for (int i = 0; i < 256; i++) {
        idt_set_gate(i, isr_stub_table[i],
                     0x8E); // P=1, DPL=0, gate=1110 (64-bit interrupt gate)
    }

    idtr.limit = sizeof(idt) - 1;
    idtr.base = (uint64_t)&idt;

    asm volatile("lidt %0" : : "m"(idtr));
}
