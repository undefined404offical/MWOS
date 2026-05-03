#ifndef _NVME_H
#define _NVME_H

#include "stdint.h"
#include "stdbool.h"
#include "stddef.h"
#include "drivers/pci.h"

// NVMe寄存器定义
#define NVME_REG_CAP        0x00    // 控制器能力
#define NVME_REG_VS         0x08    // 版本
#define NVME_REG_INTMS      0x0C    // 中断掩码设置
#define NVME_REG_INTMC      0x10    // 中断掩码清除
#define NVME_REG_CC         0x14    // 控制器配置
#define NVME_REG_CSTS       0x1C    // 控制器状态
#define NVME_REG_NSSR       0x20    // NVM子系统重置
#define NVME_REG_AQA        0x24    // Admin队列属性
#define NVME_REG_ASQ        0x28    // Admin提交队列基址
#define NVME_REG_ACQ        0x30    // Admin完成队列基址
#define NVME_REG_CMBLOC     0x38    // Controller Memory Buffer Location
#define NVME_REG_CMBSZ      0x3C    // Controller Memory Buffer Size

// NVMe命令集定义
#define NVME_OPC_ADMIN_IDENTIFY     0x06
#define NVME_OPC_ADMIN_CREATE_IOQ   0x01
#define NVME_OPC_ADMIN_DELETE_IOQ   0x00
#define NVME_OPC_IO_READ            0x02
#define NVME_OPC_IO_WRITE           0x01
#define NVME_OPC_IO_FLUSH           0x00

// NVMe队列定义
#define NVME_ADMIN_QUEUE_ID     0x00
#define NVME_IO_QUEUE_ID_BASE   0x01
#define NVME_MAX_QUEUES         64
#define NVME_QUEUE_SIZE         1024
#define NVME_MAX_QUEUE_ENTRIES  4096

// NVMe命名空间定义
#define NVME_NSID_BROADCAST     0xFFFFFFFF
#define NVME_NSID_DEFAULT       0x00000001

// NVMe命令结构
struct nvme_sq_entry {
    uint32_t cdw0;             // DW0: 命令标识符
    uint32_t nsid;             // DW1: 命名空间ID
    uint64_t reserved1;        // DW2-3: 保留
    uint64_t mptr;             // DW4-5: 元数据指针
    uint64_t prp1;             // DW6-7: PRP条目1
    uint64_t prp2;             // DW8-9: PRP条目2
    uint32_t cdw10;            // DW10: 命令特定
    uint32_t cdw11;            // DW11: 命令特定
    uint32_t cdw12;            // DW12: 命令特定
    uint32_t cdw13;            // DW13: 命令特定
    uint32_t cdw14;            // DW14: 命令特定
    uint32_t cdw15;            // DW15: 命令特定
} __attribute__((packed));

// NVMe完成队列条目结构
struct nvme_cq_entry {
    uint32_t result;           // DW0: 命令结果
    uint32_t reserved;         // DW1: 保留
    uint16_t sq_head;          // DW2: 提交队列头指针
    uint16_t sq_id;            // DW2: 提交队列ID
    uint16_t command_id;       // DW3: 命令ID
    uint16_t status;           // DW3: 状态字段
} __attribute__((packed));

// NVMe队列对结构
struct nvme_queue_pair {
    uint16_t qid;              // 队列ID
    uint16_t vector;           // 中断向量
    uint32_t size;             // 队列大小
    uint16_t sq_tail;          // 提交队列尾指针
    uint16_t cq_head;          // 完成队列头指针
    struct nvme_sq_entry *sq;  // 提交队列基址
    struct nvme_cq_entry *cq;  // 完成队列基址
    uint32_t *sq_db;           // 提交队列门铃寄存器
    uint32_t *cq_db;           // 完成队列门铃寄存器
    uint32_t phase;            // 阶段位
    bool enabled;              // 队列是否启用
};

// NVMe命名空间结构
struct nvme_namespace {
    uint32_t nsid;             // 命名空间ID
    uint64_t size;             // 命名空间大小（扇区）
    uint32_t block_size;       // 块大小
    uint32_t format;           // 格式化信息
    uint64_t capacity;         // 容量（字节）
    uint64_t utilization;      // 已使用容量（字节）
    uint8_t features[16];      // 命名空间特性
};

// NVMe控制器结构
struct nvme_controller {
    volatile uint32_t *regs;   // 控制器寄存器基址
    uint32_t num_queues;       // 支持的队列数量
    uint32_t max_entries;      // 最大队列条目数
    uint32_t doorbell_stride;  // 门铃寄存器步长
    
    // 管理队列
    struct nvme_queue_pair admin_sq;  // Admin提交队列
    struct nvme_queue_pair admin_cq;  // Admin完成队列
    
    // IO队列
    struct nvme_queue_pair io_queues[NVME_MAX_QUEUES];
    uint32_t num_io_queues;    // IO队列数量
    
    // 命名空间
    struct nvme_namespace namespaces[16];
    uint32_t num_namespaces;   // 命名空间数量
    
    // 控制器信息
    uint32_t version;          // NVMe版本
    uint64_t capabilities;     // 控制器能力
    char model[40];            // 控制器型号
    char serial[20];           // 序列号
    char firmware[8];          // 固件版本
    
    // 中断处理
    uint32_t irq_vector;       // 中断向量
    bool msi_enabled;          // MSI是否启用
};

// 批量命令上下文结构
struct nvme_batch_context {
    uint32_t active_commands;  // 活动命令位图
    uint32_t completed_commands; // 完成命令位图
    uint64_t start_time;       // 批量开始时间
    uint32_t pending_count;    // 待处理命令数
    void *user_data;           // 用户数据
    uint32_t queue_id;         // 使用的队列ID
};

// 零拷贝缓冲区结构
struct nvme_zero_copy_buffer {
    void *physical_addr;       // 物理地址
    void *virtual_addr;        // 虚拟地址
    size_t size;               // 缓冲区大小
    uint32_t ref_count;        // 引用计数
    bool locked;               // 是否锁定
};

// 函数声明
bool nvme_init(void);
bool nvme_detect(pci_device_t *dev);
int nvme_identify_controller(struct nvme_controller *ctrl);
int nvme_identify_namespace(struct nvme_controller *ctrl, uint32_t nsid);
int nvme_create_io_queue(struct nvme_controller *ctrl, uint16_t qid, uint16_t vector);
int nvme_delete_io_queue(struct nvme_controller *ctrl, uint16_t qid);

// IO操作函数
int nvme_read(struct nvme_controller *ctrl, uint32_t nsid, uint64_t lba, 
              uint32_t count, void *buffer, uint16_t qid);
int nvme_write(struct nvme_controller *ctrl, uint32_t nsid, uint64_t lba, 
               uint32_t count, const void *buffer, uint16_t qid);
int nvme_flush(struct nvme_controller *ctrl, uint32_t nsid, uint16_t qid);

// 批量操作函数
int nvme_read_batch(struct nvme_controller *ctrl, uint32_t nsid, 
                    uint64_t *lba_array, uint32_t *count_array, 
                    void **buffer_array, uint32_t cmd_count, uint16_t qid);
int nvme_write_batch(struct nvme_controller *ctrl, uint32_t nsid, 
                     uint64_t *lba_array, uint32_t *count_array, 
                     const void **buffer_array, uint32_t cmd_count, uint16_t qid);

// 零拷贝支持
int nvme_alloc_zero_copy_buffer(struct nvme_controller *ctrl, 
                                struct nvme_zero_copy_buffer *buf, size_t size);
void nvme_free_zero_copy_buffer(struct nvme_controller *ctrl, 
                               struct nvme_zero_copy_buffer *buf);
int nvme_read_zero_copy(struct nvme_controller *ctrl, uint32_t nsid, 
                       uint64_t lba, uint32_t count, 
                       struct nvme_zero_copy_buffer *buf, uint16_t qid);
int nvme_write_zero_copy(struct nvme_controller *ctrl, uint32_t nsid, 
                        uint64_t lba, uint32_t count, 
                        struct nvme_zero_copy_buffer *buf, uint16_t qid);

// 中断处理
void nvme_irq_handler(uint32_t irq, void *data);
int nvme_setup_msi(pci_device_t *dev);

// 电源管理
int nvme_set_power_state(struct nvme_controller *ctrl, uint8_t state);
int nvme_get_power_state(struct nvme_controller *ctrl, uint8_t *state);

// 性能监控
void nvme_get_perf_stats(struct nvme_controller *ctrl, 
                        uint64_t *read_ops, uint64_t *write_ops,
                        uint64_t *total_bytes, uint64_t *avg_latency);

#endif