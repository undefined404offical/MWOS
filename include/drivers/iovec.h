#ifndef _IOVEC_H
#define _IOVEC_H

#include "stdint.h"
#include "stddef.h"
#include "stdbool.h"

// 向量缓冲区结构（基于XJ380-XSK2.1的优化）
struct iovec {
    void *iov_base;    // 缓冲区基址
    size_t iov_len;    // 缓冲区长度
};

// 批量I/O请求结构
struct bulk_io_request {
    uint64_t lba;               // 起始逻辑块地址
    uint32_t sector_count;      // 扇区数量
    struct iovec *iovecs;       // 向量缓冲区数组
    uint32_t iovec_count;       // 向量缓冲区数量
    bool is_write;              // 读写标志
    uint32_t flags;             // 请求标志
};

// 批量I/O操作标志
#define BIO_FLAG_PREFETCH     0x0001  // 预读标志
#define BIO_FLAG_URGENT       0x0002  // 紧急请求
#define BIO_FLAG_SEQUENTIAL   0x0004  // 顺序访问
#define BIO_FLAG_RANDOM       0x0008  // 随机访问

// 批量I/O统计信息
struct bio_stats {
    uint64_t total_requests;    // 总请求数
    uint64_t total_sectors;     // 总扇区数
    uint64_t cache_hits;        // 缓存命中数
    uint64_t cache_misses;      // 缓存未命中数
    uint64_t sequential_reads;  // 顺序读取数
    uint64_t random_reads;      // 随机读取数
    uint32_t avg_queue_depth;   // 平均队列深度
};

// 函数声明
int bio_read_bulk(struct bulk_io_request *req);
int bio_write_bulk(struct bulk_io_request *req);
int bio_prefetch(uint64_t lba, uint32_t count);
void bio_get_stats(struct bio_stats *stats);
void bio_reset_stats(void);

// 向量缓冲区操作
int iovec_alloc(struct iovec **iovecs, uint32_t count, size_t total_size);
void iovec_free(struct iovec *iovecs, uint32_t count);
int iovec_copy_to(struct iovec *dst, uint32_t dst_count, 
                  struct iovec *src, uint32_t src_count);
int iovec_copy_from(struct iovec *dst, uint32_t dst_count, 
                    struct iovec *src, uint32_t src_count);

#endif