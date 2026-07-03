#include "serial.h"
#include "idt.h"
#include "trap.h"
#include "gdt.h"
#include "elfexev.h"

#define SYS_WRITE   0
#define SYS_EXIT    1

// main syscall dispatcher (called from idt_handler)
// tf->rax: syscall number
// tf->rdi, rsi, rdx, r10, r8, r9: args
void handle_syscall(Trapframe *tf)
{
    uint64_t nr = tf->rax;
    uint64_t ret = 0;

    switch (nr) {
    case SYS_WRITE:
    {
        // rdi=fd, rsi=buf, rdx=len
        uint64_t buf = tf->rsi;
        uint64_t len = tf->rdx;
        for (uint32_t i = 0; i < len; i++) {
            serial_putc(((char*)buf)[i]);
        }
        ret = len;
        break;
    }
    case SYS_EXIT:
        // 如果从用户态调用，跳回内核退出处理函数
        if (tf->cs == SEL_UCODE) {
            if (g_elf_ret_rip != 0) {
                tf->rip  = g_elf_ret_rip;
                tf->cs   = SEL_KCODE;
                tf->ss   = SEL_KDATA;
                tf->rsp  = g_elf_ret_rsp;
                tf->rflags = 0x202;
                serial_puts("syscall: exit -> kernel\n");
            } else {
                serial_puts("syscall: exit (no handler, halting)\n");
                while (1) asm("hlt");
            }
        }
        // 如果从内核态调用，忽略
        ret = 0;
        break;
    default:
        serial_puts("syscall: unknown nr=");
        serial_putdec64(nr);
        serial_puts("\n");
        ret = -1;
        break;
    }

    tf->rax = ret;
}
