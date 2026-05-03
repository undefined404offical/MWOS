#ifndef KLOG_H
#define KLOG_H

#include <stdbool.h>
#include <stdarg.h>

typedef enum {
    KLOG_INFO,
    KLOG_WARN,
    KLOG_ERROR
} klog_level_t;

// 由 WM 设置：
// NULL  = 图形未初始化
// false = 图形初始化但关闭屏幕日志
// true  = 打印到屏幕
extern bool* klog_to_screen;

// 核心日志函数
void klog(klog_level_t level, const char* fmt, ...);

// 便捷宏
#define kinfo(fmt, ...)  klog(KLOG_INFO,  fmt, ##__VA_ARGS__)
#define kwarn(fmt, ...)  klog(KLOG_WARN,  fmt, ##__VA_ARGS__)
#define kerror(fmt, ...) klog(KLOG_ERROR, fmt, ##__VA_ARGS__)

#endif // KLOG_H
