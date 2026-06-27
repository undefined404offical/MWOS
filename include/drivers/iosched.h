#ifndef _IOSCHED_H
#define _IOSCHED_H

#include "stdint.h"
#include "stdbool.h"
#include "drivers/disk.h"

// I/O调度器类型定义
#define IOSCHED_NOOP       0  // NOOP调度器
#define IOSCHED_CFQ        1  // 完全公平队列调度器
#define IOSCHED_DEADLINE   2  // 截止时间调度器
#define IOSCHED_ADAPTIVE   3  // 自适应调度器

// I/O请求优先级
#define IOPRIO_REALTIME    0  // 实时优先级
#define IOPRIO_NORMAL      1  // 正常优先级
#define IOPRIO_IDLE        2  // 空闲优先级

// 请求方向
#define REQ_READ           0  // 读请求
#define REQ_WRITE          1  // 写请求

// 调度器参数
#define CFQ_TIMESLICE_MS   100  // CFQ时间片（毫秒）
#define DEADLINE_READ_MS   500  // 读请求截止时间
#define DEADLINE_WRITE_MS  5000 // 写请求截止时间
#define MAX_REQUESTS       256  // 最大请求数

// I/O请求结构
struct io_request {
    uint32_t req_id;           // 请求ID
    uint32_t type;             // 请求类型（读/写）
    uint64_t sector;           // 起始扇区
    uint32_t count;            // 扇区数量
    void *buffer;              // 数据缓冲区
    uint32_t priority;         // 请求优先级
    uint64_t deadline;         // 截止时间（毫秒）
    uint64_t submit_time;      // 提交时间
    uint64_t start_time;       // 开始处理时间
    uint64_t complete_time;    // 完成时间
    struct io_request *next;   // 链表指针
    struct io_request *prev;   // 链表指针
};

// CFQ调度器结构
struct cfq_scheduler {
    struct io_request *queues[3];  // 优先级队列（实时、正常、空闲）
    uint32_t queue_sizes[3];       // 队列大小
    uint32_t current_queue;        // 当前处理的队列
    uint64_t queue_times[3];       // 队列时间片
    uint64_t last_switch_time;     // 最后切换时间
    uint32_t timeslice_ms;         // 时间片长度
};

// Deadline调度器结构
struct deadline_scheduler {
    struct io_request *read_fifo;  // 读请求FIFO队列
    struct io_request *write_fifo; // 写请求FIFO队列
    struct io_request *read_sort;  // 读请求排序队列
    struct io_request *write_sort; // 写请求排序队列
    uint32_t read_count;           // 读请求计数
    uint32_t write_count;          // 写请求计数
    uint64_t read_deadline_ms;     // 读请求截止时间
    uint64_t write_deadline_ms;    // 写请求截止时间
    uint32_t batch_size;           // 批量处理大小
};

// NOOP调度器结构
struct noop_scheduler {
    struct io_request *fifo_queue; // FIFO队列
    uint32_t queue_size;           // 队列大小
};

// 自适应调度器结构
struct adaptive_scheduler {
    uint32_t current_scheduler;    // 当前使用的调度器
    uint32_t device_type;          // 设备类型（HDD/SSD）
    uint64_t last_switch_time;     // 最后切换时间
    uint32_t read_ratio;           // 读请求比例
    uint32_t sequential_ratio;     // 顺序访问比例
    uint64_t avg_latency;          // 平均延迟
    uint32_t workload_type;        // 工作负载类型
};

// I/O调度器管理器
struct iosched_manager {
    uint32_t scheduler_type;       // 当前调度器类型
    union {
        struct cfq_scheduler cfq;      // CFQ调度器
        struct deadline_scheduler dl;  // Deadline调度器
        struct noop_scheduler noop;    // NOOP调度器
        struct adaptive_scheduler adaptive; // 自适应调度器
    } scheduler;
    
    uint32_t total_requests;       // 总请求数
    uint64_t total_latency;        // 总延迟
    uint32_t read_requests;        // 读请求数
    uint32_t write_requests;       // 写请求数
    uint32_t sequential_requests;  // 顺序请求数
    uint32_t random_requests;      // 随机请求数
};

// 函数声明
void iosched_init(uint32_t scheduler_type);
void iosched_set_scheduler(uint32_t scheduler_type);
int iosched_submit_request(uint32_t type, uint64_t sector, uint32_t count, 
                          void *buffer, uint32_t priority);
struct io_request *iosched_get_next_request(void);
void iosched_complete_request(struct io_request *req);
void iosched_cancel_request(uint32_t req_id);

// 调度器特定函数
void cfq_init(void);
void deadline_init(void);
void noop_init(void);
void adaptive_init(void);

struct io_request *cfq_get_next(void);
struct io_request *deadline_get_next(void);
struct io_request *noop_get_next(void);
struct io_request *adaptive_get_next(void);

void cfq_submit(struct io_request *req);
void deadline_submit(struct io_request *req);
void noop_submit(struct io_request *req);
void adaptive_submit(struct io_request *req);

// 性能统计
void iosched_get_stats(uint32_t *total_req, uint64_t *avg_latency, 
                      uint32_t *read_ratio, uint32_t *seq_ratio);
void iosched_reset_stats(void);

// 工作负载分析
void iosched_analyze_workload(void);
uint32_t iosched_detect_device_type(void);
uint32_t iosched_detect_workload_type(void);

// 批量操作支持
int iosched_submit_batch(uint32_t *types, uint64_t *sectors, uint32_t *counts, 
                        void **buffers, uint32_t num_reqs, uint32_t priority);

#endif