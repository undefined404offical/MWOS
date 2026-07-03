/* MWOS 示例用户程序 – hello.c
 *
 * 用法:
 *   # 在 MWOS shell 中:
 *   exec /hello.elf
 *
 * 输出:
 *   Hello from user program!
 *   Counting: 1 2 3
 */

/* 系统调用接口 */
#define SYS_WRITE   0
#define SYS_EXIT    1

static long syscall(long nr, long arg0, long arg1, long arg2)
{
    long ret;
    register long r10 asm("r10") = 0;
    asm volatile("int $48"
                 : "=a"(ret)
                 : "a"(nr), "D"(arg0), "S"(arg1), "d"(arg2), "r"(r10)
                 : "memory", "rcx", "r11");
    return ret;
}

static void putchar(char c)
{
    syscall(SYS_WRITE, 1, (long)&c, 1);
}

static void puts(const char *s)
{
    while (*s)
        putchar(*s++);
}

static void putdec(long n)
{
    char buf[20];
    int i = 0;
    if (n == 0) {
        putchar('0');
        return;
    }
    if (n < 0) {
        putchar('-');
        n = -n;
    }
    while (n > 0) {
        buf[i++] = '0' + (n % 10);
        n /= 10;
    }
    while (i > 0)
        putchar(buf[--i]);
}

/* 入口点 – 由 ELF loader 直接跳转 */
void _start(void)
{
    puts("Hello from user program!\n");

    puts("Counting: ");
    for (int i = 1; i <= 3; i++) {
        putdec(i);
        putchar(' ');
    }
    puts("\n");

    puts("Goodbye from ring 3!\n");

    /* 退出 */
    syscall(SYS_EXIT, 0, 0, 0);

    /* 如果 exit 没生效，死循环 */
    while (1)
        asm("hlt");
}
