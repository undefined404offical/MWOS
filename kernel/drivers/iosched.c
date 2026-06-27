#include "drivers/iosched.h"
#include "memory.h"
#include "serial.h"
#include "string.h"
#include "timer.h"
#include "drivers/disk.h"

static struct iosched_manager g_iosched;
static uint32_t g_next_req_id = 1;

// 全局I/O请求池
static struct io_request g_request_pool[MAX_REQUESTS];
static struct io_request *g_free_list = NULL;

// 分配I/O请求
static struct io_request *alloc_request(void) {
    if (!g_free_list) {
        // 初始化请求池
        if (g_next_req_id == 1) {
            for (int i = 0; i < MAX_REQUESTS - 1; i++) {
                g_request_pool[i].next = &g_request_pool[i + 1];
            }
            g_request_pool[MAX_REQUESTS - 1].next = NULL;
            g_free_list = &g_request_pool[0];
        } else {
            return NULL; // 请求池已满
        }
    }
    
    struct io_request *req = g_free_list;
    g_free_list = g_free_list->next;
    
    memset(req, 0, sizeof(struct io_request));
    req->req_id = g_next_req_id++;
    
    return req;
}

// 释放I/O请求
static void free_request(struct io_request *req) {
    if (!req) return;
    
    req->next = g_free_list;
    g_free_list = req;
}

// I/O调度器初始化
void iosched_init(uint32_t scheduler_type) {
    memset(&g_iosched, 0, sizeof(g_iosched));
    g_iosched.scheduler_type = scheduler_type;
    
    // 初始化请求池
    g_free_list = NULL;
    g_next_req_id = 1;
    
    // 初始化特定调度器
    switch (scheduler_type) {
        case IOSCHED_CFQ:
            cfq_init();
            serial_puts("IOSched: CFQ scheduler initialized\n");
            break;
        case IOSCHED_DEADLINE:
            deadline_init();
            serial_puts("IOSched: Deadline scheduler initialized\n");
            break;
        case IOSCHED_NOOP:
            noop_init();
            serial_puts("IOSched: NOOP scheduler initialized\n");
            break;
        case IOSCHED_ADAPTIVE:
            adaptive_init();
            serial_puts("IOSched: Adaptive scheduler initialized\n");
            break;
        default:
            g_iosched.scheduler_type = IOSCHED_NOOP;
            noop_init();
            serial_puts("IOSched: Defaulting to NOOP scheduler\n");
            break;
    }
}

// 设置调度器类型
void iosched_set_scheduler(uint32_t scheduler_type) {
    if (scheduler_type == g_iosched.scheduler_type) return;
    
    // 清理当前调度器
    switch (g_iosched.scheduler_type) {
        case IOSCHED_CFQ:
            // CFQ清理逻辑
            break;
        case IOSCHED_DEADLINE:
            // Deadline清理逻辑
            break;
        case IOSCHED_NOOP:
            // NOOP清理逻辑
            break;
        case IOSCHED_ADAPTIVE:
            // Adaptive清理逻辑
            break;
    }
    
    // 初始化新调度器
    iosched_init(scheduler_type);
}

// 提交I/O请求
int iosched_submit_request(uint32_t type, uint64_t sector, uint32_t count, 
                          void *buffer, uint32_t priority) {
    if (count == 0 || !buffer) return -1;
    
    struct io_request *req = alloc_request();
    if (!req) return -1; // 请求池已满
    
    req->type = type;
    req->sector = sector;
    req->count = count;
    req->buffer = buffer;
    req->priority = priority;
    req->submit_time = timer_get_ticks();
    
    // 设置截止时间
    if (g_iosched.scheduler_type == IOSCHED_DEADLINE) {
        if (type == REQ_READ) {
            req->deadline = req->submit_time + DEADLINE_READ_MS;
        } else {
            req->deadline = req->submit_time + DEADLINE_WRITE_MS;
        }
    }
    
    // 更新统计信息
    g_iosched.total_requests++;
    if (type == REQ_READ) {
        g_iosched.read_requests++;
    } else {
        g_iosched.write_requests++;
    }
    
    // 分析是否为顺序访问
    static uint64_t last_sector = 0;
    static uint32_t sequential_count = 0;
    
    if (last_sector != 0 && sector == last_sector + 1) {
        sequential_count++;
        if (sequential_count >= 3) {
            g_iosched.sequential_requests++;
        }
    } else {
        sequential_count = 0;
        g_iosched.random_requests++;
    }
    last_sector = sector + count - 1;
    
    // 提交到具体调度器
    switch (g_iosched.scheduler_type) {
        case IOSCHED_CFQ:
            cfq_submit(req);
            break;
        case IOSCHED_DEADLINE:
            deadline_submit(req);
            break;
        case IOSCHED_NOOP:
            noop_submit(req);
            break;
        case IOSCHED_ADAPTIVE:
            adaptive_submit(req);
            break;
    }
    
    return req->req_id;
}

// 获取下一个要处理的请求
struct io_request *iosched_get_next_request(void) {
    struct io_request *req = NULL;
    
    switch (g_iosched.scheduler_type) {
        case IOSCHED_CFQ:
            req = cfq_get_next();
            break;
        case IOSCHED_DEADLINE:
            req = deadline_get_next();
            break;
        case IOSCHED_NOOP:
            req = noop_get_next();
            break;
        case IOSCHED_ADAPTIVE:
            req = adaptive_get_next();
            break;
    }
    
    if (req) {
        req->start_time = timer_get_ticks();
    }
    
    return req;
}

// 完成请求处理
void iosched_complete_request(struct io_request *req) {
    if (!req) return;
    
    req->complete_time = timer_get_ticks();
    
    // 更新延迟统计
    uint64_t latency = req->complete_time - req->submit_time;
    g_iosched.total_latency += latency;
    
    // 执行实际的磁盘操作
    if (req->type == REQ_READ) {
        disk_read(req->sector, req->count, req->buffer);
    } else {
        disk_write(req->sector, req->count, req->buffer);
    }
    
    // 释放请求
    free_request(req);
}

// CFQ调度器实现
void cfq_init(void) {
    memset(&g_iosched.scheduler.cfq, 0, sizeof(struct cfq_scheduler));
    g_iosched.scheduler.cfq.timeslice_ms = CFQ_TIMESLICE_MS;
    g_iosched.scheduler.cfq.last_switch_time = timer_get_ticks();
}

void cfq_submit(struct io_request *req) {
    struct cfq_scheduler *cfq = &g_iosched.scheduler.cfq;
    
    // 添加到相应优先级队列
    req->next = cfq->queues[req->priority];
    if (cfq->queues[req->priority]) {
        cfq->queues[req->priority]->prev = req;
    }
    cfq->queues[req->priority] = req;
    cfq->queue_sizes[req->priority]++;
}

struct io_request *cfq_get_next(void) {
    struct cfq_scheduler *cfq = &g_iosched.scheduler.cfq;
    uint64_t current_time = timer_get_ticks();
    
    // 检查是否需要切换队列
    if (current_time - cfq->last_switch_time >= cfq->timeslice_ms) {
        cfq->current_queue = (cfq->current_queue + 1) % 3;
        cfq->last_switch_time = current_time;
    }
    
    // 从当前队列获取请求
    for (int i = 0; i < 3; i++) {
        int queue_idx = (cfq->current_queue + i) % 3;
        if (cfq->queues[queue_idx]) {
            struct io_request *req = cfq->queues[queue_idx];
            
            // 从队列中移除
            cfq->queues[queue_idx] = req->next;
            if (req->next) {
                req->next->prev = NULL;
            }
            cfq->queue_sizes[queue_idx]--;
            
            return req;
        }
    }
    
    return NULL;
}

// Deadline调度器实现
void deadline_init(void) {
    memset(&g_iosched.scheduler.dl, 0, sizeof(struct deadline_scheduler));
    g_iosched.scheduler.dl.read_deadline_ms = DEADLINE_READ_MS;
    g_iosched.scheduler.dl.write_deadline_ms = DEADLINE_WRITE_MS;
    g_iosched.scheduler.dl.batch_size = 8;
}

void deadline_submit(struct io_request *req) {
    struct deadline_scheduler *dl = &g_iosched.scheduler.dl;
    
    // 添加到FIFO队列
    if (req->type == REQ_READ) {
        req->next = dl->read_fifo;
        if (dl->read_fifo) {
            dl->read_fifo->prev = req;
        }
        dl->read_fifo = req;
        dl->read_count++;
    } else {
        req->next = dl->write_fifo;
        if (dl->write_fifo) {
            dl->write_fifo->prev = req;
        }
        dl->write_fifo = req;
        dl->write_count++;
    }
}

struct io_request *deadline_get_next(void) {
    struct deadline_scheduler *dl = &g_iosched.scheduler.dl;
    uint64_t current_time = timer_get_ticks();
    
    // 检查截止时间紧迫的请求
    struct io_request *urgent_read = NULL;
    struct io_request *urgent_write = NULL;
    
    // 查找即将超时的读请求
    struct io_request *req = dl->read_fifo;
    while (req) {
        if (req->deadline - current_time < 100) { // 100ms内超时
            urgent_read = req;
            break;
        }
        req = req->next;
    }
    
    // 查找即将超时的写请求
    req = dl->write_fifo;
    while (req) {
        if (req->deadline - current_time < 100) { // 100ms内超时
            urgent_write = req;
            break;
        }
        req = req->next;
    }
    
    // 优先处理即将超时的请求
    if (urgent_read) {
        // 从队列中移除
        if (urgent_read->prev) urgent_read->prev->next = urgent_read->next;
        if (urgent_read->next) urgent_read->next->prev = urgent_read->prev;
        if (dl->read_fifo == urgent_read) dl->read_fifo = urgent_read->next;
        dl->read_count--;
        return urgent_read;
    }
    
    if (urgent_write) {
        // 从队列中移除
        if (urgent_write->prev) urgent_write->prev->next = urgent_write->next;
        if (urgent_write->next) urgent_write->next->prev = urgent_write->prev;
        if (dl->write_fifo == urgent_write) dl->write_fifo = urgent_write->next;
        dl->write_count--;
        return urgent_write;
    }
    
    // 批量处理读请求（优化SSD性能）
    if (dl->read_count >= dl->batch_size) {
        req = dl->read_fifo;
        if (req) {
            // 从队列中移除
            if (req->prev) req->prev->next = req->next;
            if (req->next) req->next->prev = req->prev;
            if (dl->read_fifo == req) dl->read_fifo = req->next;
            dl->read_count--;
            return req;
        }
    }
    
    // 处理写请求
    if (dl->write_fifo) {
        req = dl->write_fifo;
        // 从队列中移除
        if (req->prev) req->prev->next = req->next;
        if (req->next) req->next->prev = req->prev;
        if (dl->write_fifo == req) dl->write_fifo = req->next;
        dl->write_count--;
        return req;
    }
    
    // 处理剩余的读请求
    if (dl->read_fifo) {
        req = dl->read_fifo;
        // 从队列中移除
        if (req->prev) req->prev->next = req->next;
        if (req->next) req->next->prev = req->prev;
        if (dl->read_fifo == req) dl->read_fifo = req->next;
        dl->read_count--;
        return req;
    }
    
    return NULL;
}

// NOOP调度器实现
void noop_init(void) {
    memset(&g_iosched.scheduler.noop, 0, sizeof(struct noop_scheduler));
}

void noop_submit(struct io_request *req) {
    struct noop_scheduler *noop = &g_iosched.scheduler.noop;
    
    // 添加到FIFO队列尾部
    if (!noop->fifo_queue) {
        noop->fifo_queue = req;
    } else {
        struct io_request *tail = noop->fifo_queue;
        while (tail->next) tail = tail->next;
        tail->next = req;
        req->prev = tail;
    }
    noop->queue_size++;
}

struct io_request *noop_get_next(void) {
    struct noop_scheduler *noop = &g_iosched.scheduler.noop;
    
    if (!noop->fifo_queue) return NULL;
    
    struct io_request *req = noop->fifo_queue;
    noop->fifo_queue = req->next;
    if (noop->fifo_queue) {
        noop->fifo_queue->prev = NULL;
    }
    noop->queue_size--;
    
    return req;
}

// 自适应调度器实现
void adaptive_init(void) {
    memset(&g_iosched.scheduler.adaptive, 0, sizeof(struct adaptive_scheduler));
    
    // 检测设备类型
    g_iosched.scheduler.adaptive.device_type = iosched_detect_device_type();
    
    // 初始化为适合设备类型的调度器
    if (g_iosched.scheduler.adaptive.device_type == 1) { // SSD
        g_iosched.scheduler.adaptive.current_scheduler = IOSCHED_NOOP;
    } else { // HDD
        g_iosched.scheduler.adaptive.current_scheduler = IOSCHED_CFQ;
    }
    
    g_iosched.scheduler.adaptive.last_switch_time = timer_get_ticks();
}

void adaptive_submit(struct io_request *req) {
    // 根据当前调度器提交请求
    switch (g_iosched.scheduler.adaptive.current_scheduler) {
        case IOSCHED_CFQ:
            cfq_submit(req);
            break;
        case IOSCHED_DEADLINE:
            deadline_submit(req);
            break;
        case IOSCHED_NOOP:
            noop_submit(req);
            break;
    }
    
    // 定期分析工作负载
    static uint32_t submit_count = 0;
    if (++submit_count >= 100) {
        iosched_analyze_workload();
        submit_count = 0;
    }
}

struct io_request *adaptive_get_next(void) {
    // 根据当前调度器获取请求
    switch (g_iosched.scheduler.adaptive.current_scheduler) {
        case IOSCHED_CFQ:
            return cfq_get_next();
        case IOSCHED_DEADLINE:
            return deadline_get_next();
        case IOSCHED_NOOP:
            return noop_get_next();
    }
    
    return NULL;
}

// 性能统计
void iosched_get_stats(uint32_t *total_req, uint64_t *avg_latency, 
                      uint32_t *read_ratio, uint32_t *seq_ratio) {
    if (total_req) *total_req = g_iosched.total_requests;
    
    if (avg_latency && g_iosched.total_requests > 0) {
        *avg_latency = g_iosched.total_latency / g_iosched.total_requests;
    }
    
    if (read_ratio && g_iosched.total_requests > 0) {
        *read_ratio = (g_iosched.read_requests * 100) / g_iosched.total_requests;
    }
    
    if (seq_ratio && g_iosched.total_requests > 0) {
        *seq_ratio = (g_iosched.sequential_requests * 100) / g_iosched.total_requests;
    }
}

// 工作负载分析
void iosched_analyze_workload(void) {
    struct adaptive_scheduler *adaptive = &g_iosched.scheduler.adaptive;
    
    // 计算读请求比例
    if (g_iosched.total_requests > 0) {
        adaptive->read_ratio = (g_iosched.read_requests * 100) / g_iosched.total_requests;
    }
    
    // 计算顺序访问比例
    if (g_iosched.total_requests > 0) {
        adaptive->sequential_ratio = (g_iosched.sequential_requests * 100) / g_iosched.total_requests;
    }
    
    // 计算平均延迟
    if (g_iosched.total_requests > 0) {
        adaptive->avg_latency = g_iosched.total_latency / g_iosched.total_requests;
    }
    
    // 检测工作负载类型
    adaptive->workload_type = iosched_detect_workload_type();
    
    // 根据工作负载类型调整调度器
    uint32_t new_scheduler = adaptive->current_scheduler;
    
    if (adaptive->device_type == 1) { // SSD
        if (adaptive->sequential_ratio > 70) {
            new_scheduler = IOSCHED_NOOP; // 顺序访问适合NOOP
        } else if (adaptive->read_ratio > 80) {
            new_scheduler = IOSCHED_DEADLINE; // 读密集型适合Deadline
        }
    } else { // HDD
        if (adaptive->sequential_ratio > 60) {
            new_scheduler = IOSCHED_CFQ; // 顺序访问适合CFQ
        } else if (adaptive->read_ratio > 70) {
            new_scheduler = IOSCHED_DEADLINE; // 读密集型适合Deadline
        }
    }
    
    // 切换调度器（避免频繁切换）
    if (new_scheduler != adaptive->current_scheduler && 
        timer_get_ticks() - adaptive->last_switch_time > 5000) { // 5秒冷却
        iosched_set_scheduler(new_scheduler);
        adaptive->last_switch_time = timer_get_ticks();
    }
}

// 检测设备类型（简化实现）
uint32_t iosched_detect_device_type(void) {
    // 在实际系统中，这里应该检测设备特性
    // 这里返回示例值：0=HDD, 1=SSD
    return 1; // 假设为SSD
}

// 检测工作负载类型
uint32_t iosched_detect_workload_type(void) {
    struct adaptive_scheduler *adaptive = &g_iosched.scheduler.adaptive;
    
    if (adaptive->read_ratio > 80) {
        return 1; // 读密集型
    } else if (adaptive->read_ratio < 20) {
        return 2; // 写密集型
    } else if (adaptive->sequential_ratio > 60) {
        return 3; // 顺序访问型
    } else {
        return 4; // 随机访问型
    }
}

// 批量提交请求
int iosched_submit_batch(uint32_t *types, uint64_t *sectors, uint32_t *counts, 
                        void **buffers, uint32_t num_reqs, uint32_t priority) {
    if (num_reqs == 0 || !types || !sectors || !counts || !buffers) {
        return -1;
    }
    
    for (uint32_t i = 0; i < num_reqs; i++) {
        if (iosched_submit_request(types[i], sectors[i], counts[i], 
                                  buffers[i], priority) == -1) {
            return -1;
        }
    }
    
    return 0;
}