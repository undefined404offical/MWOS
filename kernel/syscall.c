#include "serial.h"
#include "idt.h"
#include "trap.h"

#define SYS_WRITE   0
#define SYS_EXIT    1

// syscall编号对应的参数个数
static const int syscall_nargs[] = {
    3,  // SYS_WRITE: fd, buf, len
    1,  // SYS_EXIT:  code
};

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
        serial_puts("syscall: exit (ignored)\n");
        ret = 0;
        break;
    default:
        serial_puts("syscall: unknown nr=");
        serial_putdec64(nr);
        serial_puts("\n");
        ret = -1;
        break;
    }

    tf->rax = ret;  // -> iretq时rax中即为返回值
}
