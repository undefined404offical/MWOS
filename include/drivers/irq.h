#ifndef _IRQ_H
#define _IRQ_H

#include "stdint.h"
#include "stdbool.h"
#include "drivers/pci.h"

// 中断类型定义
#define IRQ_TYPE_LEGACY     0x00  // 传统中断
#define IRQ_TYPE_MSI        0x01  // 消息信号中断
#define IRQ_TYPE_MSI_X      0x02  // 扩展MSI

// 中断优先级
#define IRQ_PRIORITY_HIGH   0
#define IRQ_PRIORITY_NORMAL 1
#define IRQ_PRIORITY_LOW    2

// 中断处理函数类型
typedef void (*irq_handler_t)(uint32_t irq, void *data);

// 中断描述符结构
struct irq_desc {
    uint32_t irq;               // 中断号
    uint32_t type;              // 中断类型
    uint32_t priority;          // 中断优先级
    irq_handler_t handler;      // 中断处理函数
    void *data;                 // 处理函数数据
    uint32_t count;             // 中断计数
    uint32_t flags;             // 标志位
    char name[32];              // 中断名称
};

// MSI配置结构
struct msi_config {
    uint32_t address_lo;        // 消息地址低32位
    uint32_t address_hi;        // 消息地址高32位
    uint32_t data;              // 消息数据
    uint32_t vector;            // 中断向量
    uint32_t enabled;           // 是否启用
};

// 批量中断处理结构
struct batch_irq_context {
    uint32_t pending_irqs[32];  // 待处理中断位图
    uint32_t processed_count;   // 已处理中断计数
    uint64_t start_time;        // 处理开始时间
    uint64_t end_time;          // 处理结束时间
    uint64_t total_processing_time; // 总处理时间
    uint32_t high_priority_count;   // 高优先级中断计数
    uint32_t normal_priority_count; // 正常优先级中断计数
    uint32_t low_priority_count;    // 低优先级中断计数
};

// 函数声明
void irq_init(void);
int irq_register(uint32_t irq, irq_handler_t handler, void *data, uint32_t priority, const char *name);
void irq_unregister(uint32_t irq);
void irq_handler(uint32_t irq);

// MSI相关函数
int msi_alloc_vector(uint32_t *vector);
void msi_free_vector(uint32_t vector);
int msi_enable(pci_device_t *dev, uint32_t vector);
void msi_disable(pci_device_t *dev);

// 批量中断处理
void batch_irq_start(struct batch_irq_context *ctx);
void batch_irq_add(struct batch_irq_context *ctx, uint32_t irq);
void batch_irq_process(struct batch_irq_context *ctx);
void batch_irq_end(struct batch_irq_context *ctx);
void batch_irq_process_efficient(struct batch_irq_context *ctx);

// 中断统计
void irq_get_stats(uint32_t irq, uint32_t *count, uint64_t *total_time);
void irq_reset_stats(uint32_t irq);

// AHCI专用中断处理
void ahci_irq_handler(uint32_t irq, void *data);
int ahci_setup_msi(pci_device_t *dev);

#endif