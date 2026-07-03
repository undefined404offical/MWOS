#include "drivers/fs/fscache.h"
#include "memory.h"
#include "string.h"
#include "drivers/disk.h"
#include "serial.h"

static fscache_t g_fscache;

// 简单的哈希函数
static uint32_t hash_sector(uint32_t device_id, uint64_t sector)
{
    return (device_id ^ (sector >> 32) ^ sector) % FS_CACHE_SIZE;
}

// 从LRU链表中移除块
static void lru_remove(cache_block_t *block)
{
    if (block->prev) block->prev->next = block->next;
    if (block->next) block->next->prev = block->prev;
    
    if (block == g_fscache.lru_head) g_fscache.lru_head = block->next;
    if (block == g_fscache.lru_tail) g_fscache.lru_tail = block->prev;
    
    block->prev = NULL;
    block->next = NULL;
}

// 将块添加到LRU链表头部
static void lru_add_to_head(cache_block_t *block)
{
    block->next = g_fscache.lru_head;
    block->prev = NULL;
    
    if (g_fscache.lru_head) g_fscache.lru_head->prev = block;
    g_fscache.lru_head = block;
    
    if (!g_fscache.lru_tail) g_fscache.lru_tail = block;
}

// 将块标记为最近使用
static void lru_touch(cache_block_t *block)
{
    if (block == g_fscache.lru_head) return;
    
    lru_remove(block);
    lru_add_to_head(block);
}

// 查找缓存块
static cache_block_t *find_block(uint32_t device_id, uint64_t sector)
{
    uint32_t idx = hash_sector(device_id, sector);
    cache_block_t *block = g_fscache.blocks[idx];
    
    while (block) {
        if (block->device_id == device_id && block->sector == sector && block->valid) {
            return block;
        }
        block = block->next;
    }
    
    return NULL;
}

// 分配新的缓存块
static cache_block_t *alloc_block(void)
{
    cache_block_t *block = kmalloc(sizeof(cache_block_t));
    if (!block) return NULL;
    
    memset(block, 0, sizeof(cache_block_t));
    return block;
}

// 智能淘汰算法 - 基于XJ380-XSK2.1优化策略
static cache_block_t *smart_evict_block(void)
{
    cache_block_t *candidate = g_fscache.lru_tail;
    cache_block_t *current = candidate;
    
    // 遍历LRU链表，寻找最佳淘汰候选
    while (current) {
        // 优先淘汰非脏块
        if (!current->dirty) {
            candidate = current;
            break;
        }
        
        // 如果都是脏块，选择最久未访问的
        if (current->last_access < candidate->last_access) {
            candidate = current;
        }
        
        current = current->prev;
    }
    
    if (!candidate) return NULL;
    
    // 如果块被修改，需要写回磁盘
    if (candidate->dirty) {
        disk_write(candidate->sector, 1, candidate->data);
        candidate->dirty = false;
    }
    
    // 从哈希表中移除
    uint32_t idx = hash_sector(candidate->device_id, candidate->sector);
    cache_block_t *prev = NULL;
    cache_block_t *curr = g_fscache.blocks[idx];
    
    while (curr) {
        if (curr == candidate) {
            if (prev) prev->next = curr->next;
            else g_fscache.blocks[idx] = curr->next;
            break;
        }
        prev = curr;
        curr = curr->next;
    }
    
    lru_remove(candidate);
    candidate->valid = false;
    
    return candidate;
}

// 分析访问模式
static void analyze_access_pattern(uint64_t sector)
{
    access_pattern_t *pattern = &g_fscache.prefetch.pattern;
    
    // 添加新的访问记录
    if (pattern->count < ACCESS_PATTERN_HISTORY) {
        pattern->sectors[pattern->count] = sector;
        pattern->count++;
    } else {
        pattern->sectors[pattern->index] = sector;
        pattern->index = (pattern->index + 1) % ACCESS_PATTERN_HISTORY;
    }
    
    // 分析是否为顺序访问
    if (pattern->count >= 2) {
        uint64_t prev_sector = pattern->sectors[(pattern->index + ACCESS_PATTERN_HISTORY - 1) % ACCESS_PATTERN_HISTORY];
        
        if (sector == prev_sector + 1) {
            pattern->sequential_count++;
            if (pattern->sequential_count >= 3) {
                pattern->sequential = true;
                // 动态调整预读大小
                g_fscache.prefetch.prefetch_size = pattern->sequential_count;
                if (g_fscache.prefetch.prefetch_size > PREFETCH_WINDOW) {
                    g_fscache.prefetch.prefetch_size = PREFETCH_WINDOW;
                }
            }
        } else {
            pattern->sequential_count = 0;
            pattern->sequential = false;
            g_fscache.prefetch.prefetch_size = 1;
        }
    }
    
    g_fscache.prefetch.last_sector = sector;
}

// 执行智能预读
static void perform_prefetch(uint32_t device_id, uint64_t sector)
{
    if (!g_fscache.prefetch.prefetch_enabled) return;
    
    access_pattern_t *pattern = &g_fscache.prefetch.pattern;
    
    if (pattern->sequential && pattern->sequential_count >= 3) {
        // 执行预读
        uint32_t prefetch_count = g_fscache.prefetch.prefetch_size;
        
        for (uint32_t i = 1; i <= prefetch_count; i++) {
            uint64_t prefetch_sector = sector + i;
            
            // 检查是否已经在缓存中
            cache_block_t *block = find_block(device_id, prefetch_sector);
            if (!block) {
                // 预读数据
                uint8_t temp_buffer[SECTOR_SIZE];
                if (disk_read(prefetch_sector, 1, temp_buffer)) {
                    // 将预读数据添加到缓存
                    fscache_write_sector(device_id, prefetch_sector, temp_buffer);
                }
            }
        }
    }
}

void fscache_init(void)
{
    memset(&g_fscache, 0, sizeof(g_fscache));
    
    // 初始化预读上下文
    g_fscache.prefetch.prefetch_enabled = true;
    g_fscache.prefetch.prefetch_size = 1;
    g_fscache.max_cache_size = FS_CACHE_SIZE;
    g_fscache.cache_size = FS_CACHE_SIZE / 2; // 初始缓存大小
    
    serial_puts("FSCache: Smart prefetch enabled\n");
}

bool fscache_read_sector(uint32_t device_id, uint64_t sector, uint8_t *buffer)
{
    g_fscache.access_count++;
    
    // 分析访问模式
    analyze_access_pattern(sector);
    
    cache_block_t *block = find_block(device_id, sector);
    if (block) {
        // 缓存命中
        memcpy(buffer, block->data, SECTOR_SIZE);
        lru_touch(block);
        g_fscache.hit_count++;
        
        // 执行智能预读
        perform_prefetch(device_id, sector);
        return true;
    }
    
    // 缓存未命中，从磁盘读取（disk_read 返回 0 表示成功）
    if (disk_read((uint32_t)sector, 1, buffer) != 0) {
        g_fscache.miss_count++;
        return false;
    }
    
    // 将数据添加到缓存
    cache_block_t *new_block = alloc_block();
    if (!new_block) {
        // 缓存已满，淘汰一个块
        new_block = smart_evict_block();
        if (!new_block) {
            g_fscache.miss_count++;
            
            // 即使未缓存也执行预读
            perform_prefetch(device_id, sector);
            return true; // 读取成功，但未缓存
        }
    }
    
    new_block->device_id = device_id;
    new_block->sector = sector;
    memcpy(new_block->data, buffer, SECTOR_SIZE);
    new_block->valid = true;
    new_block->dirty = false;
    new_block->last_access = g_fscache.access_count;
    
    // 添加到哈希表和LRU链表
    uint32_t idx = hash_sector(device_id, sector);
    new_block->next = g_fscache.blocks[idx];
    if (g_fscache.blocks[idx]) g_fscache.blocks[idx]->prev = new_block;
    g_fscache.blocks[idx] = new_block;
    
    lru_add_to_head(new_block);
    
    g_fscache.miss_count++;
    
    // 执行智能预读
    perform_prefetch(device_id, sector);
    return true;
}

bool fscache_write_sector(uint32_t device_id, uint64_t sector, const uint8_t *buffer)
{
    cache_block_t *block = find_block(device_id, sector);
    
    if (block) {
        // 更新缓存块
        memcpy(block->data, buffer, SECTOR_SIZE);
        block->dirty = true;
        lru_touch(block);
    } else {
        // 分配新缓存块
        cache_block_t *new_block = alloc_block();
        if (!new_block) {
            new_block = smart_evict_block();
            if (!new_block) {
                // 直接写入磁盘
                return disk_write(sector, 1, buffer);
            }
        }
        
        new_block->device_id = device_id;
        new_block->sector = sector;
        memcpy(new_block->data, buffer, SECTOR_SIZE);
        new_block->valid = true;
        new_block->dirty = true;
        new_block->last_access = g_fscache.access_count;
        
        // 添加到哈希表和LRU链表
        uint32_t idx = hash_sector(device_id, sector);
        new_block->next = g_fscache.blocks[idx];
        if (g_fscache.blocks[idx]) g_fscache.blocks[idx]->prev = new_block;
        g_fscache.blocks[idx] = new_block;
        
        lru_add_to_head(new_block);
    }
    
    return true;
}

void fscache_flush(void)
{
    cache_block_t *block = g_fscache.lru_head;
    
    while (block) {
        if (block->dirty) {
            disk_write(block->sector, 1, block->data);
            block->dirty = false;
        }
        block = block->next;
    }
}

void fscache_stats(uint32_t *hit, uint32_t *miss, uint32_t *total)
{
    if (hit) *hit = g_fscache.hit_count;
    if (miss) *miss = g_fscache.miss_count;
    if (total) *total = g_fscache.access_count;
}

// 启用/禁用预读功能
void fscache_enable_prefetch(bool enable)
{
    g_fscache.prefetch.prefetch_enabled = enable;
    
    serial_puts("FSCache: Prefetch ");
    serial_puts(enable ? "enabled" : "disabled");
    serial_puts("\n");
}

// 手动预读扇区
bool fscache_prefetch_sectors(uint32_t device_id, uint64_t start_sector, uint32_t count)
{
    if (count == 0 || count > PREFETCH_WINDOW) return false;
    
    for (uint32_t i = 0; i < count; i++) {
        uint64_t sector = start_sector + i;
        
        // 检查是否已经在缓存中
        cache_block_t *block = find_block(device_id, sector);
        if (!block) {
            // 预读数据
            uint8_t temp_buffer[SECTOR_SIZE];
            if (disk_read(sector, 1, temp_buffer)) {
                // 将预读数据添加到缓存
                fscache_write_sector(device_id, sector, temp_buffer);
            } else {
                return false;
            }
        }
    }
    
    serial_puts("FSCache: Prefetched ");
    serial_putdec32(count);
    serial_puts(" sectors from ");
    serial_putdec64(start_sector);
    serial_puts("\n");
    
    return true;
}

// 分析访问模式（外部接口）
void fscache_analyze_pattern(uint64_t sector)
{
    analyze_access_pattern(sector);
}

// 调整缓存大小
void fscache_adjust_cache_size(void)
{
    // 基于命中率动态调整缓存大小（避免浮点运算）
    uint32_t total_access = g_fscache.access_count;
    if (total_access < 100) return; // 数据不足，不调整
    
    // 使用整数运算计算命中率百分比
    uint32_t hit_percent = (g_fscache.hit_count * 100) / total_access;
    
    if (hit_percent > 80) {
        // 高命中率，增加缓存大小
        if (g_fscache.cache_size < g_fscache.max_cache_size) {
            g_fscache.cache_size += 10;
            if (g_fscache.cache_size > g_fscache.max_cache_size) {
                g_fscache.cache_size = g_fscache.max_cache_size;
            }
        }
    } else if (hit_percent < 30) {
        // 低命中率，减少缓存大小
        if (g_fscache.cache_size > 10) {
            g_fscache.cache_size -= 5;
        }
    }
    
    serial_puts("FSCache: Adjusted cache size to ");
    serial_putdec32(g_fscache.cache_size);
    serial_puts(", hit rate: ");
    serial_putdec32(hit_percent);
    serial_puts("%\n");
}

// 批量读取多个扇区
bool fscache_read_multiple(uint32_t device_id, uint64_t start_sector, uint32_t count, uint8_t *buffer)
{
    if (count == 0) return false;
    
    for (uint32_t i = 0; i < count; i++) {
        uint64_t sector = start_sector + i;
        uint8_t *sector_buffer = buffer + (i * SECTOR_SIZE);
        
        if (!fscache_read_sector(device_id, sector, sector_buffer)) {
            return false;
        }
    }
    
    // 分析批量访问模式
    analyze_access_pattern(start_sector);
    
    return true;
}

// 批量写入多个扇区
bool fscache_write_multiple(uint32_t device_id, uint64_t start_sector, uint32_t count, const uint8_t *buffer)
{
    if (count == 0) return false;
    
    for (uint32_t i = 0; i < count; i++) {
        uint64_t sector = start_sector + i;
        const uint8_t *sector_buffer = buffer + (i * SECTOR_SIZE);
        
        if (!fscache_write_sector(device_id, sector, sector_buffer)) {
            return false;
        }
    }
    
    return true;
}