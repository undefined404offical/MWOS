// shell.h - MWOS Modern Shell Header
#ifndef SHELL_H
#define SHELL_H

#include "kernel.h"
#include "serial.h"

// Shell配置
#define MAX_INPUT_LENGTH 512
#define MAX_HISTORY_SIZE 100
#define MAX_COMMANDS 50
#define MAX_ARGS 20

// 终端输出函数类型
typedef void (*term_output_func)(const char*);

// Shell函数声明
void shell_init(void);
void shell_process_char(char c);
void shell_print_prompt(void);
void shell_print(const char *str);
void shell_printf(const char* fmt, ...);
void shell_set_term_output(term_output_func func);

#endif // SHELL_H