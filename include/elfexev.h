#ifndef ELFEXEV_H
#define ELFEXEV_H

#include "cstd.h"

/* 前向声明 – 目前内核还没有真正的进程控制块 */
struct proc;

/* elfexec – 加载 ELF 文件并切换到用户态
 *
 * p    – 当前进程控制块（暂未使用，传 NULL）
 * path – ELF 文件路径
 * argv – 命令行参数（暂未使用）
 * envp – 环境变量（暂未使用）
 *
 * 返回 0 表示成功，-1 表示失败
 */
int elfexec(struct proc *p,
            const char *path,
            char *const argv[],
            char *const envp[]);

/* 被 syscall (SYS_EXIT) 用来把用户程序跳回内核 */
extern uint64_t g_elf_ret_rip;
extern uint64_t g_elf_ret_rsp;

/* 退出回调 – elf_exit_handler 在 halt 前调用（注册 shell_print_prompt 等） */
extern void (*g_elf_on_exit)(void);

#endif /* ELFEXEV_H */
