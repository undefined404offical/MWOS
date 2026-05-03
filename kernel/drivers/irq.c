#include "drivers/irq.h"
#include "drivers/pci.h"
#include "drivers/pic.h"
#include "drivers/ahci.h"
#include "memory.h"
#include "string.h"
#include "serial.h"
#include "io.h"
#include "timer.h"
#include "stdbool.h"

#define MAX_IRQS 256
static struct irq_desc g_irq_descs[MAX_IRQS];
static bool g_irq_initialized = false;

// PIC相关常量定义（从MWOS的pic.c中复制）
#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1
#define PIC_EOI      0x20

// 简单的PIC中断屏蔽/取消屏蔽函数
static void pic_mask_irq(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t mask = inb(port);
    outb(port, mask | (1 << (irq & 7)));
}

static void pic_unmask_irq(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t mask = inb(port);
    outb(port, mask & ~(1 << (irq & 7)));
}

// 初始化中断系统
void irq_init(void) {
    if (g_irq_initialized) return;
    
    // 清零所有中断描述符
    memset(g_irq_descs, 0, sizeof(g_irq_descs));
    
    // 初始化PIC - 在MWOS中，我们使用pic_remap函数
    pic_remap(0x20, 0x28);
    
    g_irq_initialized = true;
    serial_puts("IRQ: Interrupt system initialized\n");
}

// 注册中断处理函数
int irq_register(uint32_t irq, irq_handler_t handler, void *data, uint32_t priority, const char *name) {
    if (irq >= MAX_IRQS || !handler) {
        return -1;
    }
    
    struct irq_desc *desc = &g_irq_descs[irq];
    
    // 检查是否已注册
    if (desc->handler) {
        serial_puts("IRQ: Interrupt ");
        serial_putdec32(irq);
        serial_puts(" already registered\n");
        return -1;
    }
    
    desc->irq = irq;
    desc->handler = handler;
    desc->data = data;
    desc->priority = priority;
    desc->count = 0;
    desc->flags = 0;
    
    if (name) {
        strncpy(desc->name, name, sizeof(desc->name) - 1);
        desc->name[sizeof(desc->name) - 1] = '\0';
    } else {
        strcpy(desc->name, "unknown");
    }
    
    // 启用中断（如果是PIC中断）
    if (irq < 16) {
        pic_unmask_irq(irq);
    }
    
    serial_puts("IRQ: Registered handler for IRQ ");
    serial_putdec32(irq);
    serial_puts(" (");
    serial_puts(desc->name);
    serial_puts(")\n");
    
    return 0;
}

// 取消中断注册
void irq_unregister(uint32_t irq) {
    if (irq >= MAX_IRQS) return;
    
    struct irq_desc *desc = &g_irq_descs[irq];
    
    if (desc->handler) {
        // 禁用中断（如果是PIC中断）
        if (irq < 16) {
            pic_mask_irq(irq);
        }
        
        serial_puts("IRQ: Unregistered handler for IRQ ");
        serial_putdec32(irq);
        serial_puts("\n");
        
        memset(desc, 0, sizeof(struct irq_desc));
    }
}

// 通用中断处理函数
void irq_handler(uint32_t irq) {
    if (irq >= MAX_IRQS) return;
    
    struct irq_desc *desc = &g_irq_descs[irq];
    
    if (desc->handler) {
        desc->count++;
        desc->handler(irq, desc->data);
    } else {
        serial_puts("IRQ: No handler for IRQ ");
        serial_putdec32(irq);
        serial_puts("\n");
    }
    
    // 发送EOI（如果是PIC中断）
    if (irq >= 8 && irq < 16) {
        outb(PIC2_COMMAND, PIC_EOI);
    }
    if (irq < 16) {
        outb(PIC1_COMMAND, PIC_EOI);
    }
}

// MSI向量管理
#define MSI_BASE_VECTOR 0x20
#define MSI_MAX_VECTORS 32
static uint32_t g_msi_vector_bitmap = 0; // 位图管理32个MSI向量

// 分配MSI向量
int msi_alloc_vector(uint32_t *vector) {
    if (!vector) return -1;
    
    // 查找第一个空闲向量
    for (uint32_t i = 0; i < MSI_MAX_VECTORS; i++) {
        if (!(g_msi_vector_bitmap & (1 << i))) {
            g_msi_vector_bitmap |= (1 << i);
            *vector = MSI_BASE_VECTOR + i;
            
            serial_puts("MSI: Allocated vector ");
            serial_putdec32(*vector);
            serial_puts("\n");
            
            return 0;
        }
    }
    
    serial_puts("MSI: No available MSI vectors\n");
    return -1;
}

// 释放MSI向量
void msi_free_vector(uint32_t vector) {
    if (vector < MSI_BASE_VECTOR || vector >= MSI_BASE_VECTOR + MSI_MAX_VECTORS) {
        return;
    }
    
    uint32_t index = vector - MSI_BASE_VECTOR;
    g_msi_vector_bitmap &= ~(1 << index);
    
    serial_puts("MSI: Freed vector ");
    serial_putdec32(vector);
    serial_puts("\n");
}

// 启用设备的MSI
int msi_enable(pci_device_t *dev, uint32_t vector) {
    if (!dev || vector < MSI_BASE_VECTOR || vector >= MSI_BASE_VECTOR + MSI_MAX_VECTORS) {
        return -1;
    }
    
    // 在MWOS中，MSI支持有限，我们简化实现
    // 假设设备支持MSI，并直接使用固定的MSI配置
    
    // 设置MSI地址（简化处理，使用固定地址）
    uint32_t msi_address = 0xFEE00000; // Local APIC地址
    
    // 设置MSI数据（向量号）
    uint32_t msi_data = vector;
    
    // 启用MSI（简化实现，直接设置相关寄存器）
    // 在真实系统中，这里需要配置PCI配置空间
    
    serial_puts("MSI: Enabled MSI for device, vector=");
    serial_putdec32(vector);
    serial_puts(" (simulated)\n");
    
    return 0;
}

// 禁用设备的MSI
void msi_disable(pci_device_t *dev) {
    if (!dev) return;
    
    // 在MWOS中简化实现
    serial_puts("MSI: Disabled MSI for device (simulated)\n");
}

// 优化的批量中断处理
static uint64_t get_timestamp(void) {
    // 在MWOS中，我们需要使用系统提供的计时器
    // 这里简化处理，返回一个递增的计数器
    static uint64_t counter = 0;
    return counter++;
}

// 按优先级处理中断的辅助函数
static void process_irq_by_priority(struct batch_irq_context *ctx, uint32_t priority) {
    if (!ctx) return;
    
    // 处理指定优先级的中断
    for (int i = 0; i < MAX_IRQS; i++) {
        if (ctx->pending_irqs[i / 32] & (1 << (i % 32))) {
            // 检查中断优先级
            if (i < MAX_IRQS && g_irq_descs[i].priority == priority) {
                uint64_t start_time = get_timestamp();
                
                irq_handler(i);
                
                uint64_t end_time = get_timestamp();
                
                // 更新处理时间统计
                if (i < MAX_IRQS) {
                    g_irq_descs[i].count++;
                }
                
                ctx->pending_irqs[i / 32] &= ~(1 << (i % 32));
                ctx->processed_count++;
                
                // 记录处理时间（简化处理）
                ctx->total_processing_time += (end_time - start_time);
            }
        }
    }
}

void batch_irq_start(struct batch_irq_context *ctx) {
    if (ctx) {
        memset(ctx->pending_irqs, 0, sizeof(ctx->pending_irqs));
        ctx->processed_count = 0;
        ctx->start_time = get_timestamp();
        ctx->total_processing_time = 0;
        ctx->high_priority_count = 0;
        ctx->normal_priority_count = 0;
        ctx->low_priority_count = 0;
    }
}

void batch_irq_add(struct batch_irq_context *ctx, uint32_t irq) {
    if (ctx && irq < MAX_IRQS) {
        ctx->pending_irqs[irq / 32] |= (1 << (irq % 32));
        
        // 统计优先级分布
        if (irq < MAX_IRQS) {
            switch (g_irq_descs[irq].priority) {
                case IRQ_PRIORITY_HIGH:
                    ctx->high_priority_count++;
                    break;
                case IRQ_PRIORITY_NORMAL:
                    ctx->normal_priority_count++;
                    break;
                case IRQ_PRIORITY_LOW:
                    ctx->low_priority_count++;
                    break;
            }
        }
    }
}

void batch_irq_process(struct batch_irq_context *ctx) {
    if (!ctx) return;
    
    // 按优先级顺序处理中断：高 -> 正常 -> 低
    process_irq_by_priority(ctx, IRQ_PRIORITY_HIGH);
    process_irq_by_priority(ctx, IRQ_PRIORITY_NORMAL);
    process_irq_by_priority(ctx, IRQ_PRIORITY_LOW);
    
    // 处理剩余的中断（无优先级信息）
    for (int i = 0; i < MAX_IRQS; i++) {
        if (ctx->pending_irqs[i / 32] & (1 << (i % 32))) {
            irq_handler(i);
            ctx->pending_irqs[i / 32] &= ~(1 << (i % 32));
            ctx->processed_count++;
        }
    }
}

void batch_irq_end(struct batch_irq_context *ctx) {
    if (ctx) {
        ctx->end_time = get_timestamp();
        
        // 输出批量处理统计信息
        serial_puts("IRQ: Batch processed ");
        serial_putdec32(ctx->processed_count);
        serial_puts(" interrupts (High: ");
        serial_putdec32(ctx->high_priority_count);
        serial_puts(", Normal: ");
        serial_putdec32(ctx->normal_priority_count);
        serial_puts(", Low: ");
        serial_putdec32(ctx->low_priority_count);
        serial_puts(")\n");
    }
}

// 高效的批量中断处理（优化版本）
void batch_irq_process_efficient(struct batch_irq_context *ctx) {
    if (!ctx) return;
    
    // 使用位扫描技术快速处理中断
    for (uint32_t word = 0; word < (MAX_IRQS + 31) / 32; word++) {
        uint32_t pending = ctx->pending_irqs[word];
        
        while (pending) {
            // 找到最低位设置的位
            uint32_t bit = __builtin_ctz(pending);
            uint32_t irq = word * 32 + bit;
            
            if (irq < MAX_IRQS) {
                // 根据优先级决定是否立即处理
                if (g_irq_descs[irq].priority == IRQ_PRIORITY_HIGH) {
                    // 高优先级中断立即处理
                    irq_handler(irq);
                } else {
                    // 其他优先级的中断可以延迟处理
                    // 这里简化处理，立即执行
                    irq_handler(irq);
                }
                
                ctx->processed_count++;
            }
            
            // 清除已处理的位
            pending &= ~(1 << bit);
        }
        
        // 清除整个字
        ctx->pending_irqs[word] = 0;
    }
}

// 中断统计
void irq_get_stats(uint32_t irq, uint32_t *count, uint64_t *total_time) {
    if (irq >= MAX_IRQS) return;
    
    struct irq_desc *desc = &g_irq_descs[irq];
    if (count) *count = desc->count;
    if (total_time) *total_time = 0; // 简化处理
}

void irq_reset_stats(uint32_t irq) {
    if (irq >= MAX_IRQS) return;
    
    struct irq_desc *desc = &g_irq_descs[irq];
    desc->count = 0;
}

// AHCI专用中断处理
static struct ahci_hba *g_ahci_hba_ptr = NULL;

void ahci_irq_handler(uint32_t irq, void *data) {
    if (!g_ahci_hba_ptr) return;
    
    // 读取全局中断状态
    uint32_t is = g_ahci_hba_ptr->base[HBA_RIS];
    
    // 检查每个端口的完成状态
    for (uint32_t port_num = 0; port_num < g_ahci_hba_ptr->num_ports; port_num++) {
        struct hba_port *port = &g_ahci_hba_ptr->ports[port_num];
        if (!port->regs) continue;
        
        uint32_t port_is = port->regs[HBA_RPxIS];
        
        // 处理命令完成中断
        if (port_is & HBA_PxIS_DHRS) {
            // 命令完成中断
            uint32_t ci = port->regs[HBA_RPxCI];
            uint32_t sact = port->regs[HBA_RPxSACT];
            
            // 检查每个槽位的完成状态
            for (int slot = 0; slot < port->cmd_slots; slot++) {
                if (!(ci & (1 << slot)) && (sact & (1 << slot))) {
                    // 槽位已完成
                    if (port->parallel_ctx) {
                        port->parallel_ctx->completed_slots |= (1 << slot);
                        port->parallel_ctx->pending_count--;
                    }
                    
                    // 清除活动位
                    port->regs[HBA_RPxSACT] &= ~(1 << slot);
                }
            }
            
            // 清除中断状态
            port->regs[HBA_RPxIS] = port_is;
        }
        
        // 处理错误中断
        if (port_is & (HBA_PxIS_TFES | HBA_PxIS_HBFS | HBA_PxIS_HBDS | HBA_PxIS_IFS)) {
            serial_puts("AHCI: Port ");
            serial_putdec32(port_num);
            serial_puts(" error interrupt: 0x");
            serial_puthex32(port_is);
            serial_puts("\n");
            
            // 清除错误中断
            port->regs[HBA_RPxIS] = port_is;
        }
    }
    
    // 清除全局中断状态
    g_ahci_hba_ptr->base[HBA_RIS] = is;
}

int ahci_setup_msi(pci_device_t *dev) {
    if (!dev) return -1;
    
    // 分配MSI向量
    uint32_t msi_vector;
    if (msi_alloc_vector(&msi_vector) < 0) {
        return -1;
    }
    
    // 注册中断处理函数
    if (irq_register(msi_vector, ahci_irq_handler, NULL, IRQ_PRIORITY_HIGH, "ahci_msi") < 0) {
        msi_free_vector(msi_vector);
        return -1;
    }
    
    // 启用MSI
    if (msi_enable(dev, msi_vector) < 0) {
        irq_unregister(msi_vector);
        msi_free_vector(msi_vector);
        return -1;
    }
    
    // 设置AHCI全局中断使能
    if (g_ahci_hba_ptr) {
        g_ahci_hba_ptr->base[HBA_RGHC] |= (1 << 1); // 启用全局中断
    }
    
    serial_puts("AHCI: MSI setup completed, vector=");
    serial_putdec32(msi_vector);
    serial_puts("\n");
    
    return 0;
}

// 设置AHCI HBA指针（供AHCI驱动调用）
void ahci_set_hba_ptr(struct ahci_hba *hba) {
    g_ahci_hba_ptr = hba;
}