#ifndef _MWOS_SYSCALL_H
#define _MWOS_SYSCALL_H

/* MWOS 系统调用接口 – 用户程序使用 */

#define SYS_WRITE   0   /* rdi=fd, rsi=buf, rdx=len */
#define SYS_EXIT    1   /* rdi=exit_code */

static inline long syscall(long nr, long arg0, long arg1, long arg2)
{
    long ret;
    register long r10 asm("r10") = 0;  /* 不使用第4个参数 */
    asm volatile("int $48"
                 : "=a"(ret)
                 : "a"(nr), "D"(arg0), "S"(arg1), "d"(arg2), "r"(r10)
                 : "memory", "rcx", "r11");
    return ret;
}

static inline long sys_write(int fd, const void *buf, unsigned long len)
{
    return syscall(SYS_WRITE, fd, (long)buf, len);
}

static inline void sys_exit(int code)
{
    syscall(SYS_EXIT, code, 0, 0);
    while (1) asm("hlt");  /* 以防万一 */
}

#endif /* _MWOS_SYSCALL_H */
