#ifndef TTF_CACHE_H
#define TTF_CACHE_H

#include "ttf.h"
#include <stdint.h>
#include <stdbool.h>

#define TTF_CACHE_SIZE 256  // 缓存字符数量
#define TTF_GLYPH_CACHE_SIZE 1024 // 字形缓存大小

// 预渲染字符缓存项
typedef struct ttf_cache_entry {
    uint32_t codepoint;      // Unicode码点
    uint32_t font_id;        // 字体标识
    uint16_t pixel_size;     // 像素大小
    uint32_t color;          // 颜色
    
    // 渲染结果
    uint8_t *bitmap;         // 位图数据
    int width;               // 位图宽度
    int height;              // 位图高度
    int bearing_x;           // 水平偏移
    int bearing_y;           // 垂直偏移
    int advance;             // 前进宽度
    
    uint32_t last_access;    // 最后访问时间
    struct ttf_cache_entry *prev; // LRU链表前驱
    struct ttf_cache_entry *next; // LRU链表后继
} ttf_cache_entry_t;

// 字体缓存管理器
typedef struct {
    ttf_cache_entry_t *entries[TTF_CACHE_SIZE]; // 哈希表
    ttf_cache_entry_t *lru_head;                // LRU链表头
    ttf_cache_entry_t *lru_tail;                // LRU链表尾
    
    uint32_t hit_count;      // 命中次数
    uint32_t miss_count;     // 未命中次数
    uint32_t access_count;   // 总访问次数
    
    // 预渲染的ASCII字符集
    uint8_t ascii_cache[128][16][16]; // ASCII字符位图缓存 (16x16像素)
    bool ascii_loaded[128];           // ASCII字符是否已加载
} ttf_cache_t;

// 函数声明
void ttf_cache_init(void);
void ttf_cache_cleanup(void);

bool ttf_cache_get_glyph(const TTF_Font *font, uint32_t codepoint, 
                         uint16_t pixel_size, uint32_t color,
                         uint8_t **bitmap, int *width, int *height,
                         int *bearing_x, int *bearing_y, int *advance);

void ttf_cache_put_glyph(const TTF_Font *font, uint32_t codepoint,
                        uint16_t pixel_size, uint32_t color,
                        uint8_t *bitmap, int width, int height,
                        int bearing_x, int bearing_y, int advance);

void ttf_cache_preload_ascii(TTF_Font *font, uint16_t pixel_size, uint32_t color);
void ttf_cache_flush(void);
void ttf_cache_stats(uint32_t *hit, uint32_t *miss, uint32_t *total);

#endif // TTF_CACHE_H