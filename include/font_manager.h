#ifndef FONT_MANAGER_H
#define FONT_MANAGER_H

#include "ttf.h"
#include <stdint.h>
#include <stdbool.h>

#define FONT_CACHE_SIZE 16  // 最大缓存字体数量

// 字体缓存项
typedef struct {
    char path[256];          // 字体文件路径
    TTF_Font *font;          // 已加载的字体
    uint32_t ref_count;      // 引用计数
    uint32_t last_access;    // 最后访问时间
    bool valid;              // 字体是否有效
} font_cache_entry_t;

// 字体管理器
typedef struct {
    font_cache_entry_t fonts[FONT_CACHE_SIZE];
    uint32_t font_count;
    uint32_t access_count;
} font_manager_t;

// 函数声明
void font_manager_init(void);
TTF_Font *font_manager_load_font(const char *path);
void font_manager_unload_font(const char *path);
void font_manager_cleanup(void);
TTF_Font *font_manager_get_font(const char *path);
void font_manager_stats(uint32_t *loaded, uint32_t *cached);

#endif // FONT_MANAGER_H