#include "ttf_cache.h"
#include "memory.h"
#include "string.h"
#include "ttf.h"

static ttf_cache_t g_ttf_cache;

// 简单的哈希函数
static uint32_t hash_glyph(uint32_t font_id, uint32_t codepoint, uint16_t pixel_size, uint32_t color)
{
    return (font_id ^ codepoint ^ pixel_size ^ color) % TTF_CACHE_SIZE;
}

// 从LRU链表中移除项
static void lru_remove_entry(ttf_cache_entry_t *entry)
{
    if (entry->prev) entry->prev->next = entry->next;
    if (entry->next) entry->next->prev = entry->prev;
    
    if (entry == g_ttf_cache.lru_head) g_ttf_cache.lru_head = entry->next;
    if (entry == g_ttf_cache.lru_tail) g_ttf_cache.lru_tail = entry->prev;
    
    entry->prev = NULL;
    entry->next = NULL;
}

// 将项添加到LRU链表头部
static void lru_add_to_head(ttf_cache_entry_t *entry)
{
    entry->next = g_ttf_cache.lru_head;
    entry->prev = NULL;
    
    if (g_ttf_cache.lru_head) g_ttf_cache.lru_head->prev = entry;
    g_ttf_cache.lru_head = entry;
    
    if (!g_ttf_cache.lru_tail) g_ttf_cache.lru_tail = entry;
}

// 将项标记为最近使用
static void lru_touch_entry(ttf_cache_entry_t *entry)
{
    if (entry == g_ttf_cache.lru_head) return;
    
    lru_remove_entry(entry);
    lru_add_to_head(entry);
}

// 查找缓存项
static ttf_cache_entry_t *find_cache_entry(uint32_t font_id, uint32_t codepoint, 
                                          uint16_t pixel_size, uint32_t color)
{
    uint32_t idx = hash_glyph(font_id, codepoint, pixel_size, color);
    ttf_cache_entry_t *entry = g_ttf_cache.entries[idx];
    
    while (entry) {
        if (entry->font_id == font_id && 
            entry->codepoint == codepoint &&
            entry->pixel_size == pixel_size &&
            entry->color == color) {
            return entry;
        }
        entry = entry->next;
    }
    
    return NULL;
}

// 分配新的缓存项
static ttf_cache_entry_t *alloc_cache_entry(void)
{
    ttf_cache_entry_t *entry = kmalloc(sizeof(ttf_cache_entry_t));
    if (!entry) return NULL;
    
    memset(entry, 0, sizeof(ttf_cache_entry_t));
    return entry;
}

// 淘汰最久未使用的项
static ttf_cache_entry_t *evict_lru_entry(void)
{
    ttf_cache_entry_t *victim = g_ttf_cache.lru_tail;
    if (!victim) return NULL;
    
    // 从哈希表中移除
    uint32_t idx = hash_glyph(victim->font_id, victim->codepoint, 
                             victim->pixel_size, victim->color);
    ttf_cache_entry_t *prev = NULL;
    ttf_cache_entry_t *curr = g_ttf_cache.entries[idx];
    
    while (curr) {
        if (curr == victim) {
            if (prev) prev->next = curr->next;
            else g_ttf_cache.entries[idx] = curr->next;
            break;
        }
        prev = curr;
        curr = curr->next;
    }
    
    lru_remove_entry(victim);
    
    // 释放位图内存
    if (victim->bitmap) {
        kfree(victim->bitmap);
    }
    
    return victim;
}

void ttf_cache_init(void)
{
    memset(&g_ttf_cache, 0, sizeof(g_ttf_cache));
    memset(g_ttf_cache.ascii_loaded, 0, sizeof(g_ttf_cache.ascii_loaded));
}

void ttf_cache_cleanup(void)
{
    ttf_cache_entry_t *entry = g_ttf_cache.lru_head;
    
    while (entry) {
        ttf_cache_entry_t *next = entry->next;
        if (entry->bitmap) {
            kfree(entry->bitmap);
        }
        kfree(entry);
        entry = next;
    }
    
    memset(&g_ttf_cache, 0, sizeof(g_ttf_cache));
}

bool ttf_cache_get_glyph(const TTF_Font *font, uint32_t codepoint, 
                         uint16_t pixel_size, uint32_t color,
                         uint8_t **bitmap, int *width, int *height,
                         int *bearing_x, int *bearing_y, int *advance)
{
    g_ttf_cache.access_count++;
    
    // 检查ASCII预渲染缓存
    if (codepoint < 128 && g_ttf_cache.ascii_loaded[codepoint]) {
        *bitmap = (uint8_t*)g_ttf_cache.ascii_cache[codepoint];
        *width = 16;
        *height = 16;
        *bearing_x = 0;
        *bearing_y = 16;
        *advance = 16;
        g_ttf_cache.hit_count++;
        return true;
    }
    
    // 检查常规缓存
    ttf_cache_entry_t *entry = find_cache_entry((uint32_t)font, codepoint, pixel_size, color);
    if (entry) {
        *bitmap = entry->bitmap;
        *width = entry->width;
        *height = entry->height;
        *bearing_x = entry->bearing_x;
        *bearing_y = entry->bearing_y;
        *advance = entry->advance;
        lru_touch_entry(entry);
        g_ttf_cache.hit_count++;
        return true;
    }
    
    g_ttf_cache.miss_count++;
    return false;
}

void ttf_cache_put_glyph(const TTF_Font *font, uint32_t codepoint,
                         uint16_t pixel_size, uint32_t color,
                         uint8_t *bitmap, int width, int height,
                         int bearing_x, int bearing_y, int advance)
{
    // 如果是ASCII字符，存储到预渲染缓存
    if (codepoint < 128) {
        if (width <= 16 && height <= 16) {
            memset(g_ttf_cache.ascii_cache[codepoint], 0, 16*16);
            for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x++) {
                    g_ttf_cache.ascii_cache[codepoint][y][x] = bitmap[y * width + x];
                }
            }
            g_ttf_cache.ascii_loaded[codepoint] = true;
        }
    }
    
    // 分配新的缓存项
    ttf_cache_entry_t *new_entry = alloc_cache_entry();
    if (!new_entry) {
        // 缓存已满，淘汰一个项
        new_entry = evict_lru_entry();
        if (!new_entry) return; // 缓存已满，无法添加
    }
    
    new_entry->font_id = (uint32_t)font;
    new_entry->codepoint = codepoint;
    new_entry->pixel_size = pixel_size;
    new_entry->color = color;
    
    // 复制位图数据
    int bitmap_size = width * height;
    new_entry->bitmap = kmalloc(bitmap_size);
    if (new_entry->bitmap) {
        memcpy(new_entry->bitmap, bitmap, bitmap_size);
    }
    
    new_entry->width = width;
    new_entry->height = height;
    new_entry->bearing_x = bearing_x;
    new_entry->bearing_y = bearing_y;
    new_entry->advance = advance;
    new_entry->last_access = g_ttf_cache.access_count;
    
    // 添加到哈希表和LRU链表
    uint32_t idx = hash_glyph(new_entry->font_id, new_entry->codepoint, 
                             new_entry->pixel_size, new_entry->color);
    new_entry->next = g_ttf_cache.entries[idx];
    if (g_ttf_cache.entries[idx]) g_ttf_cache.entries[idx]->prev = new_entry;
    g_ttf_cache.entries[idx] = new_entry;
    
    lru_add_to_head(new_entry);
}

void ttf_cache_preload_ascii(TTF_Font *font, uint16_t pixel_size, uint32_t color)
{
    // 预渲染ASCII字符集
    for (uint32_t cp = 32; cp < 127; cp++) { // 可打印ASCII字符
        if (!g_ttf_cache.ascii_loaded[cp]) {
            // 渲染字符并缓存
            uint16_t glyph = ttf_char_to_glyph(font, cp);
            if (glyph != 0) {
                // 这里需要调用TTF渲染函数来获取字形数据
                // 暂时留空，实际实现需要TTF渲染函数的支持
            }
        }
    }
}

void ttf_cache_flush(void)
{
    ttf_cache_cleanup();
    ttf_cache_init();
}

void ttf_cache_stats(uint32_t *hit, uint32_t *miss, uint32_t *total)
{
    if (hit) *hit = g_ttf_cache.hit_count;
    if (miss) *miss = g_ttf_cache.miss_count;
    if (total) *total = g_ttf_cache.access_count;
}