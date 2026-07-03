#include "memory.h"
#include "font_manager.h"
#include "ttf.h"
#include "string.h"

static font_manager_t g_font_manager;

void font_manager_init(void)
{
    memset(&g_font_manager, 0, sizeof(g_font_manager));
}

TTF_Font *font_manager_load_font(const char *path)
{
    if (!path) return NULL;
    
    g_font_manager.access_count++;
    
    // 检查字体是否已缓存
    for (uint32_t i = 0; i < g_font_manager.font_count; i++) {
        font_cache_entry_t *entry = &g_font_manager.fonts[i];
        if (entry->valid && strcmp(entry->path, path) == 0) {
            entry->ref_count++;
            entry->last_access = g_font_manager.access_count;
            return entry->font;
        }
    }
    
    // 加载新字体
    TTF_Font *font = ttf_load_from_path(path);
    if (!font) {
        return NULL;
    }
    
    // 查找空闲槽位或淘汰最久未使用的字体
    font_cache_entry_t *entry = NULL;
    uint32_t oldest_index = 0;
    uint32_t oldest_access = g_font_manager.access_count;
    
    if (g_font_manager.font_count < FONT_CACHE_SIZE) {
        entry = &g_font_manager.fonts[g_font_manager.font_count];
        g_font_manager.font_count++;
    } else {
        // 淘汰最久未使用的字体
        for (uint32_t i = 0; i < g_font_manager.font_count; i++) {
            if (g_font_manager.fonts[i].last_access < oldest_access) {
                oldest_access = g_font_manager.fonts[i].last_access;
                oldest_index = i;
            }
        }
        
        entry = &g_font_manager.fonts[oldest_index];
        
        // 卸载旧字体
        if (entry->font) {
            ttf_unload(entry->font);
        }
    }
    
    // 初始化新字体缓存项
    strncpy(entry->path, path, sizeof(entry->path) - 1);
    entry->path[sizeof(entry->path) - 1] = '\0';
    entry->font = font;
    entry->ref_count = 1;
    entry->last_access = g_font_manager.access_count;
    entry->valid = true;
    
    return font;
}

void font_manager_unload_font(const char *path)
{
    if (!path) return;
    
    for (uint32_t i = 0; i < g_font_manager.font_count; i++) {
        font_cache_entry_t *entry = &g_font_manager.fonts[i];
        if (entry->valid && strcmp(entry->path, path) == 0) {
            entry->ref_count--;
            
            if (entry->ref_count == 0) {
                // 引用计数为0，可以卸载字体
                if (entry->font) {
                ttf_unload(entry->font);
            }
                entry->valid = false;
                
                // 如果这是最后一个字体，减少字体计数
                if (i == g_font_manager.font_count - 1) {
                    g_font_manager.font_count--;
                }
            }
            
            break;
        }
    }
}

TTF_Font *font_manager_get_font(const char *path)
{
    if (!path) return NULL;
    
    g_font_manager.access_count++;
    
    for (uint32_t i = 0; i < g_font_manager.font_count; i++) {
        font_cache_entry_t *entry = &g_font_manager.fonts[i];
        if (entry->valid && strcmp(entry->path, path) == 0) {
            entry->last_access = g_font_manager.access_count;
            return entry->font;
        }
    }
    
    return NULL;
}

void font_manager_cleanup(void)
{
    for (uint32_t i = 0; i < g_font_manager.font_count; i++) {
        font_cache_entry_t *entry = &g_font_manager.fonts[i];
        if (entry->valid && entry->font) {
            ttf_unload(entry->font);
        }
    }
    
    memset(&g_font_manager, 0, sizeof(g_font_manager));
}

void font_manager_stats(uint32_t *loaded, uint32_t *cached)
{
    if (loaded) *loaded = g_font_manager.font_count;
    if (cached) *cached = FONT_CACHE_SIZE;
}