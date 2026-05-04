// shell.c - MWOS Modern Shell Implementation
#include "shell.h"
#include "memory.h"
#include "string.h"
#include "serial.h"
#include "klog.h"

// 简单的字符串处理函数（MWOS可能没有完整的标准库）
static void shell_memmove(char *dest, const char *src, size_t n) {
    if (dest < src) {
        for (size_t i = 0; i < n; i++) {
            dest[i] = src[i];
        }
    } else {
        for (size_t i = n; i > 0; i--) {
            dest[i-1] = src[i-1];
        }
    }
}

static char* shell_strtok(char *str, const char *delim) {
    static char *last = NULL;
    char *start;
    
    if (str) {
        last = str;
    }
    
    if (!last || !*last) {
        return NULL;
    }
    
    // 跳过分隔符
    while (*last) {
        const char *d = delim;
        while (*d) {
            if (*last == *d) {
                last++;
                break;
            }
            d++;
        }
        if (!*d) break;
    }
    
    if (!*last) {
        return NULL;
    }
    
    start = last;
    
    // 找到下一个分隔符
    while (*last) {
        const char *d = delim;
        while (*d) {
            if (*last == *d) {
                *last = '\0';
                last++;
                return start;
            }
            d++;
        }
        last++;
    }
    
    return start;
}

// Shell配置
#define MAX_INPUT_LENGTH 512
#define MAX_HISTORY_SIZE 100
#define MAX_COMMANDS 50
#define MAX_ARGS 20

// Shell状态结构
typedef struct {
    char cwd[256];
    char username[32];
    char hostname[32];
    char input_buffer[MAX_INPUT_LENGTH];
    int input_length;
    int cursor_pos;
    
    // 命令历史
    char history[MAX_HISTORY_SIZE][MAX_INPUT_LENGTH];
    int history_count;
    int history_index;
    
    // 自动补全
    char completion_buffer[MAX_INPUT_LENGTH];
    char *completion_list[100];
    int completion_count;
    int completion_index;
    
    // 命令表
    struct {
        char *name;
        void (*func)(int argc, char **argv);
        char *description;
    } commands[MAX_COMMANDS];
    int command_count;
    
    term_output_func term_output;
    bool initialized;
} shell_state_t;

static shell_state_t g_shell;

// 简单的路径解析函数（解决链接错误）
void resolve_path(const char *path, char *full_path) {
    if (path[0] == '/') {
        // 绝对路径
        strcpy(full_path, path);
    } else {
        // 相对路径
        strcpy(full_path, g_shell.cwd);
        if (strcmp(g_shell.cwd, "/") != 0) {
            strcat(full_path, "/");
        }
        strcat(full_path, path);
    }
}

// 内置命令函数声明
static void cmd_help(int argc, char **argv);
static void cmd_echo(int argc, char **argv);
static void cmd_clear(int argc, char **argv);
static void cmd_ls(int argc, char **argv);
static void cmd_cd(int argc, char **argv);
static void cmd_pwd(int argc, char **argv);
static void cmd_cat(int argc, char **argv);
static void cmd_exec(int argc, char **argv);
static void cmd_history(int argc, char **argv);
static void cmd_exit(int argc, char **argv);
static void cmd_whoami(int argc, char **argv);
static void cmd_uname(int argc, char **argv);
static void cmd_date(int argc, char **argv);
static void cmd_neofetch(int argc, char **argv);

// 辅助函数声明
static void shell_register_command(const char *name, void (*func)(int, char**), const char *desc);
static void shell_execute_line(const char *line);
static void shell_parse_command(const char *cmd_line, int *argc, char **argv);
static void shell_add_history(const char *cmd);
static void shell_show_completions(void);
static void shell_auto_complete(void);
static void shell_handle_control_char(char c);

void shell_init(void)
{
    memset(&g_shell, 0, sizeof(g_shell));
    strcpy(g_shell.cwd, "/");
    strcpy(g_shell.username, "user");
    strcpy(g_shell.hostname, "mwos");
    g_shell.cursor_pos = 0;
    g_shell.history_index = -1;
    g_shell.initialized = true;
    
    shell_register_command("help", cmd_help, "显示帮助信息");
    shell_register_command("echo", cmd_echo, "输出文本");
    shell_register_command("clear", cmd_clear, "清屏");
    shell_register_command("ls", cmd_ls, "列出目录内容");
    shell_register_command("cd", cmd_cd, "切换目录");
    shell_register_command("pwd", cmd_pwd, "显示当前目录");
    shell_register_command("cat", cmd_cat, "显示文件内容");
    shell_register_command("exec", cmd_exec, "执行程序");
    shell_register_command("history", cmd_history, "显示命令历史");
    shell_register_command("exit", cmd_exit, "退出shell");
    shell_register_command("whoami", cmd_whoami, "显示当前用户");
    shell_register_command("uname", cmd_uname, "显示系统信息");
    shell_register_command("date", cmd_date, "显示日期时间");
    shell_register_command("neofetch", cmd_neofetch, "显示系统信息图形");
    
    kinfo("SHELL", "Modern shell initialized with %d commands", g_shell.command_count);
    
    if (g_shell.term_output) {
        shell_print("\033[1;32mWelcome to MWOS Shell v1.0\033[0m\n");
        shell_print("Type '\033[1;33mhelp\033[0m' for available commands\n\n");
        shell_print_prompt();
    }
}

void shell_set_term_output(term_output_func func)
{
    g_shell.term_output = func;
}

void shell_process_char(char c)
{
    if (!g_shell.initialized || !g_shell.term_output) return;
    
    // 处理控制字符
    if (c < 32 || c == 127) {
        shell_handle_control_char(c);
        return;
    }
    
    // 处理可打印字符
    if (c >= 32 && c <= 126) {
        if (g_shell.input_length < MAX_INPUT_LENGTH - 1) {
            g_shell.input_buffer[g_shell.input_length] = c;
            g_shell.input_length++;
            g_shell.input_buffer[g_shell.input_length] = '\0';
            
            char buf[2] = {c, 0};
            g_shell.term_output(buf);
        }
    }
    
    #ifdef DEBUG_SERIAL
    if (c >= 32 && c <= 126) {
        serial_putc(c);
    }
    #endif
}

static void shell_handle_control_char(char c)
{
    switch (c) {
        case '\n':
        case '\r':
            if (g_shell.input_length > 0) {
                g_shell.term_output("\n");
                g_shell.input_buffer[g_shell.input_length] = '\0';
                
                shell_add_history(g_shell.input_buffer);
                shell_execute_line(g_shell.input_buffer);
                
                g_shell.input_length = 0;
                g_shell.input_buffer[0] = '\0';
                g_shell.history_index = -1;
            }
            shell_print_prompt();
            break;
            
        case '\b':
        case 127:
            if (g_shell.input_length > 0) {
                g_shell.input_length--;
                g_shell.input_buffer[g_shell.input_length] = '\0';
                g_shell.term_output("\b");
            }
            break;
            
        case '\t':
            shell_auto_complete();
            break;
            
        case 27:
            break;
    }
}

static void shell_register_command(const char *name, void (*func)(int, char**), const char *desc)
{
    if (g_shell.command_count < MAX_COMMANDS) {
        g_shell.commands[g_shell.command_count].name = (char*)name;
        g_shell.commands[g_shell.command_count].func = func;
        g_shell.commands[g_shell.command_count].description = (char*)desc;
        g_shell.command_count++;
    }
}

static void shell_execute_line(const char *line)
{
    if (!line || !*line) return;
    
    kinfo("SHELL", "Executing: %s", line);
    
    // 简单的命令解析（支持分号分隔）
    char *commands[10];
    int cmd_count = 0;
    char *line_copy = kmalloc(strlen(line) + 1);
    strcpy(line_copy, line);
    
    char *token = shell_strtok(line_copy, ";");
    while (token && cmd_count < 10) {
        commands[cmd_count++] = token;
        token = shell_strtok(NULL, ";");
    }
    
    for (int i = 0; i < cmd_count; i++) {
        // 解析参数
        int argc = 0;
        char *argv[MAX_ARGS];
        shell_parse_command(commands[i], &argc, argv);
        
        if (argc > 0) {
            // 查找并执行命令
            bool found = false;
            for (int j = 0; j < g_shell.command_count; j++) {
                if (strcmp(argv[0], g_shell.commands[j].name) == 0) {
                    g_shell.commands[j].func(argc, argv);
                    found = true;
                    break;
                }
            }
            
            if (!found) {
                shell_printf("Command not found: %s\n", argv[0]);
            }
        }
    }
    
    kfree(line_copy);
}

static void shell_parse_command(const char *cmd_line, int *argc, char **argv)
{
    *argc = 0;
    char *line_copy = kmalloc(strlen(cmd_line) + 1);
    strcpy(line_copy, cmd_line);
    
    char *token = shell_strtok(line_copy, " \t\n\r");
    while (token && *argc < MAX_ARGS) {
        argv[(*argc)++] = token;
        token = shell_strtok(NULL, " \t\n\r");
    }
    
    // 注意：line_copy需要在调用后释放
}

static void shell_add_history(const char *cmd)
{
    if (g_shell.history_count < MAX_HISTORY_SIZE) {
        strcpy(g_shell.history[g_shell.history_count], cmd);
        g_shell.history_count++;
    } else {
        // 历史记录已满，移除最旧的记录
        for (int i = 1; i < MAX_HISTORY_SIZE; i++) {
            strcpy(g_shell.history[i-1], g_shell.history[i]);
        }
        strcpy(g_shell.history[MAX_HISTORY_SIZE-1], cmd);
    }
}

static void shell_auto_complete(void)
{
    // 简单的自动补全实现
    // 这里可以扩展为文件路径补全等高级功能
    shell_print("\nAuto-completion feature coming soon...\n");
    shell_print_prompt();
    if (g_shell.input_length > 0) {
        g_shell.term_output(g_shell.input_buffer);
        for (int i = g_shell.input_length - g_shell.cursor_pos; i > 0; i--) {
            g_shell.term_output("\b");
        }
    }
}

void shell_print_prompt(void)
{
    // Ubuntu风格提示符: user@hostname:cwd$ (带颜色)
    // 普通用户: 绿色用户名@绿色主机名:蓝色路径\$
    // root用户: 红色用户名@红色主机名:蓝色路径#
    
    const char *user_color = "\033[1;32m";  // 亮绿色
    const char *host_color = "\033[1;32m";  // 亮绿色
    const char *path_color = "\033[1;34m";  // 亮蓝色
    const char *reset = "\033[0m";
    
    // 显示用户名@主机名:路径$
    shell_printf("%s%s%s@%s%s%s:%s%s%s%s$ %s",
                 user_color, g_shell.username, reset,
                 host_color, g_shell.hostname, reset,
                 path_color, g_shell.cwd, reset,
                 reset);
}

void shell_print(const char *str)
{
    if (!str || !g_shell.term_output) return;
    g_shell.term_output(str);
    
    #ifdef DEBUG_SERIAL
    serial_puts(str);
    #endif
}

void shell_printf(const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    
    // 简单的格式化输出
    int len = 0;
    const char *p = fmt;
    char *out = buf;
    
    while (*p && len < 511) {
        if (*p == '%') {
            p++;
            switch (*p) {
                case 's': {
                    char *str = va_arg(args, char*);
                    while (*str && len < 511) {
                        *out++ = *str++;
                        len++;
                    }
                    break;
                }
                case 'd': {
                    int num = va_arg(args, int);
                    char num_buf[32];
                    char *num_ptr = num_buf;
                    
                    if (num < 0) {
                        *out++ = '-';
                        len++;
                        num = -num;
                    }
                    
                    do {
                        *num_ptr++ = '0' + (num % 10);
                        num /= 10;
                    } while (num > 0);
                    
                    while (num_ptr > num_buf) {
                        *out++ = *(--num_ptr);
                        len++;
                    }
                    break;
                }
                default:
                    *out++ = '%';
                    *out++ = *p;
                    len += 2;
                    break;
            }
        } else {
            *out++ = *p;
            len++;
        }
        p++;
    }
    
    *out = '\0';
    va_end(args);
    
    shell_print(buf);
}

// ==================== 内置命令实现 ====================

static void cmd_help(int argc, char **argv)
{
    shell_print("\033[1mMWOS Modern Shell - Available Commands:\033[0m\n");
    shell_print("========================================\n");
    
    for (int i = 0; i < g_shell.command_count; i++) {
        shell_printf("  \033[32m%-10s\033[0m - %s\n", 
                    g_shell.commands[i].name, 
                    g_shell.commands[i].description);
    }
    shell_print("\n");
}

static void cmd_echo(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        shell_print(argv[i]);
        if (i < argc - 1) shell_print(" ");
    }
    shell_print("\n");
}

static void cmd_clear(int argc, char **argv)
{
    g_shell.term_output("\033[2J"); // ANSI清屏序列
}

static void cmd_ls(int argc, char **argv)
{
    char path[256];
    if (argc > 1) {
        strcpy(path, argv[1]);
    } else {
        strcpy(path, g_shell.cwd);
    }
    
    // 简单的目录列表实现
    shell_printf("Listing directory: %s\n", path);
    shell_print("(File system listing coming soon...)\n");
}

static void cmd_cd(int argc, char **argv)
{
    if (argc < 2) {
        strcpy(g_shell.cwd, "/");
    } else {
        // 简单的路径处理
        if (argv[1][0] == '/') {
            strcpy(g_shell.cwd, argv[1]);
        } else {
            // 相对路径处理
            if (strcmp(g_shell.cwd, "/") != 0) {
                strcat(g_shell.cwd, "/");
            }
            strcat(g_shell.cwd, argv[1]);
        }
        
        // 规范化路径
        // 这里可以添加路径规范化逻辑
    }
    
    shell_printf("Current directory: %s\n", g_shell.cwd);
}

static void cmd_pwd(int argc, char **argv)
{
    shell_printf("%s\n", g_shell.cwd);
}

static void cmd_cat(int argc, char **argv)
{
    if (argc < 2) {
        shell_print("Usage: cat <filename>\n");
        return;
    }
    
    shell_printf("Displaying file: %s\n", argv[1]);
    shell_print("(File content display coming soon...)\n");
}

static void cmd_exec(int argc, char **argv)
{
    if (argc < 2) {
        shell_print("Usage: exec <program> [args...]\n");
        return;
    }
    
    shell_printf("Executing program: %s\n", argv[1]);
    shell_print("(Program execution coming soon...)\n");
}

static void cmd_history(int argc, char **argv)
{
    shell_print("\033[1mCommand History:\033[0m\n");
    shell_print("================\n");
    
    int start = 0;
    int count = g_shell.history_count;
    
    if (argc > 1) {
        // 简单的字符串转数字实现
                count = 0;
                char *p = argv[1];
                while (*p >= '0' && *p <= '9') {
                    count = count * 10 + (*p - '0');
                    p++;
                }
        if (count > g_shell.history_count) {
            count = g_shell.history_count;
        }
        start = g_shell.history_count - count;
    }
    
    for (int i = start; i < g_shell.history_count; i++) {
        shell_printf("%3d: %s\n", i + 1, g_shell.history[i]);
    }
}

static void cmd_exit(int argc, char **argv)
{
    shell_print("Exiting MWOS shell...\n");
    // 这里可以添加退出逻辑
}

static void cmd_whoami(int argc, char **argv)
{
    shell_printf("%s\n", g_shell.username);
}

static void cmd_uname(int argc, char **argv)
{
    bool print_all = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0) {
            print_all = true;
            break;
        }
    }
    
    if (print_all || argc == 1) {
        shell_print("MWOS 1.0.0 mwos-kernel x86_64 MWOS-GNU\n");
    } else {
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-s") == 0) {
                shell_print("MWOS\n");
            } else if (strcmp(argv[i], "-n") == 0) {
                shell_print("mwos\n");
            } else if (strcmp(argv[i], "-r") == 0) {
                shell_print("1.0.0\n");
            } else if (strcmp(argv[i], "-v") == 0) {
                shell_print("MWOS Kernel v1.0\n");
            } else if (strcmp(argv[i], "-m") == 0) {
                shell_print("x86_64\n");
            }
        }
    }
}

static void cmd_date(int argc, char **argv)
{
    shell_print("Date/Time information coming soon...\n");
}

static void cmd_neofetch(int argc, char **argv)
{
    // Ubuntu风格配色
    shell_print("\033[1;32m");
    shell_print("             .-/+oossssoo+/-.              \n");
    shell_print("         `:+ssssssssssssssssss+:`          \n");
    shell_print("       -+ssssssssssssssssssyyssss+-        \n");
    shell_print("     .ossssssssssssssssss\033[0m\033[1;33mdMMMNy\033[1;32msssso.      \n");
    shell_print("    /sssssssssss\033[0m\033[1;33mhdmmNNmmyNMMMMh\033[1;32mssssss/     \n");
    shell_print("   +sssssssss\033[0m\033[1;33mhm\033[1;32myd\033[0m\033[1;33mMMMMMMMNddddy\033[1;32mssssssss+    \n");
    shell_print("  /ssssssss\033[0m\033[1;33mhNMMM\033[1;32myh\033[0m\033[1;33mhyyyyhmNMMMNh\033[1;32mssssssss/   \n");
    shell_print(" .ssssssss\033[0m\033[1;33mdMMMNh\033[1;32mssssssssss\033[0m\033[1;33mhNMMMd\033[1;32mssssssss.  \n");
    shell_print(" +ssss\033[0m\033[1;33mhhhyNMMNy\033[1;32mssssssssssss\033[0m\033[1;33myNMMMy\033[1;32msssssss+ \n");
    shell_print(" oss\033[0m\033[1;33mhNMMMNyMMMy\033[1;32mssssssssssss\033[0m\033[1;33mhMMMMMm\033[1;32mssssso \n");
    shell_print("+ssss\033[0m\033[1;33mhhhyNMMMNMMMy\033[1;32mssssssss\033[0m\033[1;33mhMMMMMMMm\033[1;32msssss+\n");
    shell_print(".ssss\033[0m\033[1;33mhhhyNMMMNMMMNh\033[1;32msssss\033[0m\033[1;33mhMMMMMMMMMm\033[1;32mssss.\n");
    shell_print(" +ssss\033[0m\033[1;33mhhhyNMMMNMMMNm\033[1;32mssss\033[0m\033[1;33myNMMMMMMMMd\033[1;32mssss+\n");
    shell_print("  osss\033[0m\033[1;33mhhhyNMMMNMMMNy\033[1;32mss\033[0m\033[1;33mhMMMMMMMMMNh\033[1;32mssso\n");
    shell_print("   -+sss\033[0m\033[1;33mhhhyNMMNMMMNh\033[1;32m\033[0m\033[1;33mdmNMMMMMMMMd\033[1;32msss-\n");
    shell_print("     `ossss\033[0m\033[1;33mhhhyNMMMNMMMNm\033[1;32m\033[0m\033[1;33myNMMMMMh\033[1;32mssso`\n");
    shell_print("       `+osss\033[0m\033[1;33mhhhyNMMMNMMNy\033[1;32m\033[0m\033[1;33mhMMMMh\033[1;32msss+`\n");
    shell_print("         `-+oss\033[0m\033[1;33mhhhyNMMMNm\033[1;32m\033[0m\033[1;33mdMMMd\033[1;32mss+-`\n");
    shell_print("             `-/+o\033[0m\033[1;33mssssso\033[1;32m\033[0m\033[1;33mo+/.'\n");
    shell_print("\033[0m\n");
    
    shell_printf("\033[1;32m%s\033[0m@\033[1;32m%s\033[0m\n", g_shell.username, g_shell.hostname);
    shell_print("------------------\n");
    shell_printf("\033[1;36mOS\033[0m: MWOS 1.0.0 x86_64\n");
    shell_printf("\033[1;36mHost\033[0m: MWOS Kernel\n");
    shell_printf("\033[1;36mKernel\033[0m: 1.0.0\n");
    shell_printf("\033[1;36mShell\033[0m: mwsh 1.0\n");
    shell_printf("\033[1;36mWM\033[0m: MWOS WM\n");
    shell_printf("\033[1;36mTerminal\033[0m: mwos-terminal\n");
    shell_printf("\033[1;36mCPU\033[0m: x86_64\n");
    shell_printf("\033[1;36mMemory\033[0m: 1024MB\n");
    shell_print("\n");
    
    // 颜色块
    shell_print("\033[40m  \033[41m  \033[42m  \033[43m  \033[44m  \033[45m  \033[46m  \033[47m  \033[0m\n");
}