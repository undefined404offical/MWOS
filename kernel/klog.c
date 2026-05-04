#include <stdarg.h>
#include <stdbool.h>
#include "serial.h"
#include "graphics.h"
#include "string.h"

extern bool *klog_to_screen;

typedef enum
{
    KLOG_INFO,
    KLOG_WARN,
    KLOG_ERROR
} klog_level_t;

#define KLOG_BUF_SIZE 1024

// ANSI 颜色
#define ANSI_RESET "\033[0m"
#define ANSI_RED "\033[31m"
#define ANSI_YELLOW "\033[33m"
#define ANSI_GREEN "\033[32m"

static const char *klog_prefix[] = {
    [KLOG_INFO] = "[INFO] ",
    [KLOG_WARN] = "[WARN] ",
    [KLOG_ERROR] = "[ERROR]"};

static const char *klog_color[] = {
    [KLOG_INFO] = ANSI_GREEN,
    [KLOG_WARN] = ANSI_YELLOW,
    [KLOG_ERROR] = ANSI_RED};

static int klog_cursor_x = 10;
static int klog_cursor_y = 10;
extern TTF_Font *g_klog_font;

// 声明终端输出函数指针（在kmain.c中定义）
extern void (*g_klog_term_output)(const char* str);
extern bool g_terminal_initialized;

void klog(klog_level_t level, const char *fmt, ...)
{
    char msg[KLOG_BUF_SIZE];
    char final[KLOG_BUF_SIZE + 64];

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    snprintf(final, sizeof(final),
             "%s%s %s\n" ANSI_RESET,
             klog_color[level],
             klog_prefix[level],
             msg);

    // 不再输出到终端窗口，只通过串口输出调试信息
    // if (g_klog_term_output && g_terminal_initialized) {
    //     g_klog_term_output(final);
    // }
    
    // 减少串口输出，只在调试模式下输出
    #ifdef DEBUG_SERIAL
    serial_puts(final);
    #endif

    if (klog_to_screen == NULL)
        return;

    if (*klog_to_screen == false)
        return;

    if (*klog_to_screen && g_klog_font)
    {

        uint32_t color = 0xFFFFFFFF; // 默认白色
        int x = klog_cursor_x;
        int y = klog_cursor_y;

        const char *p = final;

        while (*p)
        {
            if (*p == '\033' && p[1] == '[')
            {
                // 解析 ANSI
                p += 2;
                int code = 0;
                while (*p >= '0' && *p <= '9')
                {
                    code = code * 10 + (*p - '0');
                    p++;
                }
                if (*p == 'm')
                    p++;

                switch (code)
                {
                case 0:
                    color = 0xFFFFFFFF;
                    break; // reset
                case 31:
                    color = 0xFFFF0000;
                    break; // red
                case 32:
                    color = 0xFF00FF00;
                    break; // green
                case 33:
                    color = 0xFFFFFF00;
                    break; // yellow
                }
                continue;
            }

            // 普通字符
            char buf[2] = {*p, 0};
            ttf_draw_text_utf8_fb(g_klog_font, x, y, 16, color, buf);
            x += 8; // 字宽
            p++;
        }

        klog_cursor_y += 20;
    }
}

// 便捷宏
/*#define kinfo(fmt, ...)  klog(KLOG_INFO,  fmt, ##__VA_ARGS__)
#define kwarn(fmt, ...)  klog(KLOG_WARN,  fmt, ##__VA_ARGS__)
#define kerror(fmt, ...) klog(KLOG_ERROR, fmt, ##__VA_ARGS__)
*/