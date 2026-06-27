#ifndef FSCACHE_H
#define FSCACHE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define FS_CACHE_SIZE 256  // 缓存块数量
#define SECTOR_SIZE   512  // 扇区大小
#define PREFETCH_WINDOW 8  // 预读窗口大小
#define ACCESS_PATTERN_HISTORY 16 // 访问模式历史记录大小

// 缓存块结构
typedef struct cache_block {
    uint32_t device_id;     // 设备标识
    uint64_t sector;        // 扇区号
    uint8_t  data[SECTOR_SIZE]; // 数据
    bool     valid;         // 数据是否有效
    bool     dirty;         // 数据是否被修改
    uint32_t last_access;   // 最后访问时间戳
    struct cache_block *prev; // LRU链表前驱
    struct cache_block *next; // LRU链表后继
} cache_block_t;

// 访问模式分析结构
typedef struct {
    uint64_t sectors[ACCESS_PATTERN_HISTORY]; // 最近访问的扇区
    uint32_t count;                           // 有效记录数
    uint32_t index;                           // 当前索引
    bool sequential;                          // 是否为顺序访问
    uint32_t sequential_count;                // 连续顺序访问计数
} access_pattern_t;

// 预读上下文结构
typedef struct {
    uint64_t last_sector;                     // 最后访问的扇区
    uint32_t prefetch_size;                   // 当前预读大小
    bool prefetch_enabled;                    // 预读是否启用
    access_pattern_t pattern;                 // 访问模式分析
} prefetch_context_t;

// 文件系统缓存管理器
typedef struct {
    cache_block_t *blocks[FS_CACHE_SIZE]; // 哈希表
    cache_block_t *lru_head;               // LRU链表头
    cache_block_t *lru_tail;               // LRU链表尾
    uint32_t access_count;                 // 访问计数器
    uint32_t hit_count;                    // 命中计数器
    uint32_t miss_count;                   // 未命中计数器
    prefetch_context_t prefetch;           // 预读上下文
    uint32_t cache_size;                   // 动态缓存大小
    uint32_t max_cache_size;               // 最大缓存大小
} fscache_t;

// 函数声明
void fscache_init(void);
bool fscache_read_sector(uint32_t device_id, uint64_t sector, uint8_t *buffer);
bool fscache_write_sector(uint32_t device_id, uint64_t sector, const uint8_t *buffer);
void fscache_flush(void);
void fscache_stats(uint32_t *hit, uint32_t *miss, uint32_t *total);

// 智能预读功能
void fscache_enable_prefetch(bool enable);
bool fscache_prefetch_sectors(uint32_t device_id, uint64_t start_sector, uint32_t count);
void fscache_analyze_pattern(uint64_t sector);
void fscache_adjust_cache_size(void);

// 批量操作支持
bool fscache_read_multiple(uint32_t device_id, uint64_t start_sector, uint32_t count, uint8_t *buffer);
bool fscache_write_multiple(uint32_t device_id, uint64_t start_sector, uint32_t count, const uint8_t *buffer);

#endif // FSCACHE_H