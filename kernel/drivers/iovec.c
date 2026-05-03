#include "drivers/iovec.h"
#include "memory.h"
#include "string.h"
#include "serial.h"
#include "drivers/ahci.h"
#include "drivers/fs/fscache.h"

static struct bio_stats g_bio_stats = {0};

// 分配向量缓冲区
int iovec_alloc(struct iovec **iovecs, uint32_t count, size_t total_size) {
    if (!iovecs || count == 0) return -1;
    
    *iovecs = kmalloc(sizeof(struct iovec) * count);
    if (!*iovecs) return -1;
    
    memset(*iovecs, 0, sizeof(struct iovec) * count);
    
    // 计算每个缓冲区的大小
    size_t chunk_size = total_size / count;
    size_t remainder = total_size % count;
    
    for (uint32_t i = 0; i < count; i++) {
        size_t size = chunk_size + (i < remainder ? 1 : 0);
        (*iovecs)[i].iov_base = kmalloc(size);
        if (!(*iovecs)[i].iov_base) {
            // 分配失败，清理已分配的内存
            for (uint32_t j = 0; j < i; j++) {
                kfree((*iovecs)[j].iov_base);
            }
            kfree(*iovecs);
            *iovecs = NULL;
            return -1;
        }
        (*iovecs)[i].iov_len = size;
    }
    
    return 0;
}

// 释放向量缓冲区
void iovec_free(struct iovec *iovecs, uint32_t count) {
    if (!iovecs) return;
    
    for (uint32_t i = 0; i < count; i++) {
        if (iovecs[i].iov_base) {
            kfree(iovecs[i].iov_base);
        }
    }
    kfree(iovecs);
}

// 向量缓冲区拷贝（目标到源）
int iovec_copy_to(struct iovec *dst, uint32_t dst_count, 
                  struct iovec *src, uint32_t src_count) {
    if (!dst || !src) return -1;
    
    uint8_t *dst_ptr = (uint8_t*)dst[0].iov_base;
    uint8_t *src_ptr = (uint8_t*)src[0].iov_base;
    size_t dst_remaining = dst[0].iov_len;
    size_t src_remaining = src[0].iov_len;
    
    uint32_t dst_idx = 0, src_idx = 0;
    
    while (dst_idx < dst_count && src_idx < src_count) {
        size_t copy_size = (dst_remaining < src_remaining) ? dst_remaining : src_remaining;
        
        memcpy(dst_ptr, src_ptr, copy_size);
        
        dst_ptr += copy_size;
        src_ptr += copy_size;
        dst_remaining -= copy_size;
        src_remaining -= copy_size;
        
        if (dst_remaining == 0) {
            dst_idx++;
            if (dst_idx < dst_count) {
                dst_ptr = (uint8_t*)dst[dst_idx].iov_base;
                dst_remaining = dst[dst_idx].iov_len;
            }
        }
        
        if (src_remaining == 0) {
            src_idx++;
            if (src_idx < src_count) {
                src_ptr = (uint8_t*)src[src_idx].iov_base;
                src_remaining = src[src_idx].iov_len;
            }
        }
    }
    
    return 0;
}

// 向量缓冲区拷贝（源到目标）
int iovec_copy_from(struct iovec *dst, uint32_t dst_count, 
                    struct iovec *src, uint32_t src_count) {
    return iovec_copy_to(src, src_count, dst, dst_count);
}

// 批量读取操作（带缓存优化）
int bio_read_bulk(struct bulk_io_request *req) {
    if (!req || req->sector_count == 0 || !req->iovecs || req->iovec_count == 0) {
        return -1;
    }
    
    g_bio_stats.total_requests++;
    g_bio_stats.total_sectors += req->sector_count;
    
    // 检查是否为顺序访问
    if (req->flags & BIO_FLAG_SEQUENTIAL) {
        g_bio_stats.sequential_reads++;
    } else if (req->flags & BIO_FLAG_RANDOM) {
        g_bio_stats.random_reads++;
    }
    
    // 尝试从缓存读取
    uint32_t cached_sectors = 0;
    uint8_t temp_buffer[512]; // 单个扇区临时缓冲区
    
    for (uint32_t i = 0; i < req->sector_count; i++) {
        uint64_t current_lba = req->lba + i;
        
        // 检查缓存
        if (fscache_read_sector(0, current_lba, temp_buffer)) {
            cached_sectors++;
            
            // 将缓存数据复制到目标缓冲区
            uint64_t offset = i * 512;
            uint8_t *dst_ptr = NULL;
            size_t remaining = 512;
            
            // 找到对应的向量缓冲区位置
            for (uint32_t j = 0; j < req->iovec_count && remaining > 0; j++) {
                if (offset < req->iovecs[j].iov_len) {
                    dst_ptr = (uint8_t*)req->iovecs[j].iov_base + offset;
                    size_t copy_size = (remaining < req->iovecs[j].iov_len - offset) ? 
                                      remaining : req->iovecs[j].iov_len - offset;
                    
                    memcpy(dst_ptr, temp_buffer + (512 - remaining), copy_size);
                    remaining -= copy_size;
                    offset = 0; // 后续缓冲区从开始位置复制
                } else {
                    offset -= req->iovecs[j].iov_len;
                }
            }
        }
    }
    
    // 统计缓存命中率
    if (cached_sectors > 0) {
        g_bio_stats.cache_hits += cached_sectors;
    }
    
    // 如果有未命中的扇区，使用AHCI批量读取
    if (cached_sectors < req->sector_count) {
        g_bio_stats.cache_misses += (req->sector_count - cached_sectors);
        
        // 计算需要从磁盘读取的扇区范围
        uint64_t start_lba = req->lba;
        uint32_t read_count = req->sector_count;
        
        // 使用AHCI批量读取
        if (ahci_read_multiple(start_lba, read_count, (uint8_t*)req->iovecs[0].iov_base) < 0) {
            serial_puts("BIO: AHCI bulk read failed\n");
            return -1;
        }
        
        // 将数据添加到缓存
        for (uint32_t i = 0; i < read_count; i++) {
            // 这里需要实现缓存写入逻辑
            // fscache_write_sector(0, start_lba + i, (uint8_t*)req->iovecs[0].iov_base + i * 512);
        }
    }
    
    return 0;
}

// 批量写入操作
int bio_write_bulk(struct bulk_io_request *req) {
    if (!req || req->sector_count == 0 || !req->iovecs || req->iovec_count == 0) {
        return -1;
    }
    
    g_bio_stats.total_requests++;
    g_bio_stats.total_sectors += req->sector_count;
    
    // 使用AHCI批量写入
    if (ahci_write_multiple(req->lba, req->sector_count, (const uint8_t*)req->iovecs[0].iov_base) < 0) {
        serial_puts("BIO: AHCI bulk write failed\n");
        return -1;
    }
    
    // 更新缓存（如果存在）
    for (uint32_t i = 0; i < req->sector_count; i++) {
        // 这里需要实现缓存更新逻辑
        // fscache_write_sector(0, req->lba + i, (uint8_t*)req->iovecs[0].iov_base + i * 512);
    }
    
    return 0;
}

// 预读功能（基于XJ380-XSK2.1的优化）
int bio_prefetch(uint64_t lba, uint32_t count) {
    if (count == 0) return -1;
    
    // 分配预读缓冲区
    struct iovec *prefetch_buf;
    if (iovec_alloc(&prefetch_buf, 1, count * 512) < 0) {
        return -1;
    }
    
    // 创建预读请求
    struct bulk_io_request prefetch_req = {
        .lba = lba,
        .sector_count = count,
        .iovecs = prefetch_buf,
        .iovec_count = 1,
        .is_write = false,
        .flags = BIO_FLAG_PREFETCH | BIO_FLAG_SEQUENTIAL
    };
    
    // 执行预读
    int result = bio_read_bulk(&prefetch_req);
    
    // 释放预读缓冲区
    iovec_free(prefetch_buf, 1);
    
    return result;
}

// 获取统计信息
void bio_get_stats(struct bio_stats *stats) {
    if (stats) {
        memcpy(stats, &g_bio_stats, sizeof(struct bio_stats));
    }
}

// 重置统计信息
void bio_reset_stats(void) {
    memset(&g_bio_stats, 0, sizeof(struct bio_stats));
}

// 批量I/O初始化
void bio_init(void) {
    bio_reset_stats();
    serial_puts("BIO: Bulk I/O subsystem initialized\n");
}