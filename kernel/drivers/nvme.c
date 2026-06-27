#include "drivers/nvme.h"
#include "memory.h"
#include "serial.h"
#include "string.h"
#include "stdbool.h"
#include "timer.h"

static struct nvme_controller g_nvme_ctrl;
static bool g_nvme_initialized = false;

// 函数前置声明
static int nvme_init_admin_queue(void);
static int nvme_wait_completion(struct nvme_queue_pair *cq, uint16_t command_id);
static int nvme_submit_admin_cmd(struct nvme_controller *ctrl, struct nvme_sq_entry *cmd);
static int nvme_submit_io_cmd(struct nvme_controller *ctrl, struct nvme_queue_pair *qp, 
                              struct nvme_sq_entry *cmd);

// 初始化管理队列
static int nvme_init_admin_queue(void) {
    // 分配提交队列
    g_nvme_ctrl.admin_sq.sq = kmalloc(sizeof(struct nvme_sq_entry) * NVME_QUEUE_SIZE);
    if (!g_nvme_ctrl.admin_sq.sq) return -1;
    
    // 分配完成队列
    g_nvme_ctrl.admin_cq.cq = kmalloc(sizeof(struct nvme_cq_entry) * NVME_QUEUE_SIZE);
    if (!g_nvme_ctrl.admin_cq.cq) return -1;
    
    // 配置管理队列属性
    g_nvme_ctrl.regs[NVME_REG_AQA] = ((NVME_QUEUE_SIZE - 1) << 16) | (NVME_QUEUE_SIZE - 1);
    
    // 设置队列基址
    uint64_t sq_phys = (uint64_t)g_nvme_ctrl.admin_sq.sq;
    uint64_t cq_phys = (uint64_t)g_nvme_ctrl.admin_cq.cq;
    
    g_nvme_ctrl.regs[NVME_REG_ASQ] = (uint32_t)sq_phys;
    g_nvme_ctrl.regs[NVME_REG_ASQ + 1] = (uint32_t)(sq_phys >> 32);
    g_nvme_ctrl.regs[NVME_REG_ACQ] = (uint32_t)cq_phys;
    g_nvme_ctrl.regs[NVME_REG_ACQ + 1] = (uint32_t)(cq_phys >> 32);
    
    // 初始化队列状态
    g_nvme_ctrl.admin_sq.qid = NVME_ADMIN_QUEUE_ID;
    g_nvme_ctrl.admin_sq.size = NVME_QUEUE_SIZE;
    g_nvme_ctrl.admin_sq.sq_tail = 0;
    g_nvme_ctrl.admin_sq.enabled = true;
    
    g_nvme_ctrl.admin_cq.qid = NVME_ADMIN_QUEUE_ID;
    g_nvme_ctrl.admin_cq.size = NVME_QUEUE_SIZE;
    g_nvme_ctrl.admin_cq.cq_head = 0;
    g_nvme_ctrl.admin_cq.phase = 1;
    g_nvme_ctrl.admin_cq.enabled = true;
    
    return 0;
}

// 等待命令完成
static int nvme_wait_completion(struct nvme_queue_pair *cq, uint16_t command_id) {
    uint32_t timeout = 1000000;
    
    while (timeout--) {
        struct nvme_cq_entry *entry = &cq->cq[cq->cq_head];
        
        // 检查阶段位是否匹配
        if ((entry->status >> 16) == cq->phase) {
            // 检查命令ID是否匹配
            if (entry->command_id == command_id) {
                uint16_t status = entry->status & 0xFFFE;
                
                // 更新完成队列头指针
                cq->cq_head = (cq->cq_head + 1) % cq->size;
                if (cq->cq_head == 0) {
                    cq->phase = !cq->phase; // 翻转阶段位
                }
                
                // 更新门铃寄存器
                *cq->cq_db = cq->cq_head;
                
                return (status == 0) ? 0 : -1;
            }
        }
    }
    
    return -1; // 超时
}

// 提交管理命令
static int nvme_submit_admin_cmd(struct nvme_controller *ctrl, struct nvme_sq_entry *cmd) {
    if (!ctrl || !cmd) return -1;
    
    struct nvme_queue_pair *sq = &ctrl->admin_sq;
    struct nvme_queue_pair *cq = &ctrl->admin_cq;
    
    // 检查队列是否已满
    uint16_t next_tail = (sq->sq_tail + 1) % sq->size;
    if (next_tail == cq->cq_head) {
        return -1; // 队列满
    }
    
    // 复制命令到提交队列
    memcpy(&sq->sq[sq->sq_tail], cmd, sizeof(struct nvme_sq_entry));
    sq->sq_tail = next_tail;
    
    // 更新门铃寄存器
    *sq->sq_db = sq->sq_tail;
    
    // 等待命令完成
    return nvme_wait_completion(cq, cmd->cdw0 & 0xFFFF);
}

// 提交IO命令
static int nvme_submit_io_cmd(struct nvme_controller *ctrl, struct nvme_queue_pair *qp, 
                              struct nvme_sq_entry *cmd) {
    if (!ctrl || !qp || !cmd) return -1;
    
    // 检查队列是否已满
    uint16_t next_tail = (qp->sq_tail + 1) % qp->size;
    if (next_tail == qp->cq_head) {
        return -1; // 队列满
    }
    
    // 复制命令到提交队列
    memcpy(&qp->sq[qp->sq_tail], cmd, sizeof(struct nvme_sq_entry));
    qp->sq_tail = next_tail;
    
    // 更新门铃寄存器
    *qp->sq_db = qp->sq_tail;
    
    return 0;
}

// NVMe控制器初始化
bool nvme_init(void) {
    if (g_nvme_initialized) {
        return true;
    }
    
    serial_puts("NVMe: Initializing NVMe controller\n");
    
    // 查找NVMe设备
    pci_device_t *nvme_dev = NULL; // 暂时设为NULL，需要实现PCI查找功能
    
    if (!nvme_dev) {
        serial_puts("NVMe: No NVMe controller found (PCI search not implemented)\n");
        return false;
    }
    
    // 初始化控制器
    if (!nvme_detect(nvme_dev)) {
        serial_puts("NVMe: Controller detection failed\n");
        return false;
    }
    
    g_nvme_initialized = true;
    serial_puts("NVMe: Controller initialized successfully\n");
    return true;
}

// 检测NVMe控制器
bool nvme_detect(pci_device_t *dev) {
    // 简化实现，假设NVMe控制器存在
    // 在实际系统中，这里应该通过PCI配置空间检测NVMe设备
    
    serial_puts("NVMe: Assuming NVMe controller exists at default address\n");
    
    // 使用假设的寄存器地址
    g_nvme_ctrl.regs = (volatile uint32_t*)0xFEB80000; // 假设的NVMe寄存器地址
    
    // 读取控制器版本
    g_nvme_ctrl.version = g_nvme_ctrl.regs[NVME_REG_VS];
    serial_puts("NVMe: Version: 0x");
    serial_puthex32(g_nvme_ctrl.version);
    serial_puts("\n");
    
    // 读取控制器能力
    g_nvme_ctrl.capabilities = ((uint64_t)g_nvme_ctrl.regs[NVME_REG_CAP + 1] << 32) | 
                               g_nvme_ctrl.regs[NVME_REG_CAP];
    
    // 计算队列参数
    uint32_t mqes = (g_nvme_ctrl.capabilities & 0xFFFF) + 1;
    g_nvme_ctrl.max_entries = (mqes < NVME_MAX_QUEUE_ENTRIES) ? mqes : NVME_MAX_QUEUE_ENTRIES;
    
    uint32_t num_queues = ((g_nvme_ctrl.capabilities >> 32) & 0xFFFF) + 1;
    g_nvme_ctrl.num_queues = (num_queues < NVME_MAX_QUEUES) ? num_queues : NVME_MAX_QUEUES;
    
    serial_puts("NVMe: Max entries: ");
    serial_putdec32(g_nvme_ctrl.max_entries);
    serial_puts(", Max queues: ");
    serial_putdec32(g_nvme_ctrl.num_queues);
    serial_puts("\n");
    
    // 重置控制器
    g_nvme_ctrl.regs[NVME_REG_CC] = 0;
    while (g_nvme_ctrl.regs[NVME_REG_CSTS] & 0x01) {
        // 等待重置完成
    }
    
    // 配置控制器
    uint32_t cc = g_nvme_ctrl.regs[NVME_REG_CC];
    cc &= ~0xFFFF0000; // 清除队列大小字段
    cc |= (g_nvme_ctrl.max_entries - 1) << 16; // 设置队列大小
    cc |= 0x00460000; // 设置I/O命令集和仲裁机制
    g_nvme_ctrl.regs[NVME_REG_CC] = cc;
    
    // 启用控制器
    cc |= 0x01;
    g_nvme_ctrl.regs[NVME_REG_CC] = cc;
    
    // 等待控制器就绪
    uint32_t timeout = 1000000;
    while (!(g_nvme_ctrl.regs[NVME_REG_CSTS] & 0x01)) {
        if (--timeout == 0) {
            serial_puts("NVMe: Controller ready timeout\n");
            return false;
        }
    }
    
    // 初始化管理队列
    if (nvme_init_admin_queue() != 0) {
        serial_puts("NVMe: Admin queue initialization failed\n");
        return false;
    }
    
    // 识别控制器
    if (nvme_identify_controller(&g_nvme_ctrl) != 0) {
        serial_puts("NVMe: Controller identification failed\n");
        return false;
    }
    
    // 识别命名空间
    if (nvme_identify_namespace(&g_nvme_ctrl, NVME_NSID_DEFAULT) != 0) {
        serial_puts("NVMe: Namespace identification failed\n");
        return false;
    }
    
    // 设置MSI中断
    if (nvme_setup_msi(dev) == 0) {
        serial_puts("NVMe: MSI interrupt enabled\n");
    } else {
        serial_puts("NVMe: Using legacy interrupt mode\n");
    }
    
    return true;
}

// 识别控制器
int nvme_identify_controller(struct nvme_controller *ctrl) {
    if (!ctrl) return -1;
    
    // 分配识别数据缓冲区
    uint8_t *identify_data = kmalloc(4096);
    if (!identify_data) return -1;
    
    // 构建识别命令
    struct nvme_sq_entry cmd = {0};
    cmd.cdw0 = (NVME_OPC_ADMIN_IDENTIFY << 8) | 1; // 命令ID
    cmd.nsid = 0; // 控制器识别使用NSID 0
    cmd.prp1 = (uint64_t)identify_data;
    cmd.cdw10 = 1; // CNS=1 表示控制器识别
    
    // 发送命令
    if (nvme_submit_admin_cmd(ctrl, &cmd) != 0) {
        kfree(identify_data);
        return -1;
    }
    
    // 解析识别数据
    memcpy(ctrl->model, identify_data + 24, 40);
    memcpy(ctrl->serial, identify_data + 4, 20);
    memcpy(ctrl->firmware, identify_data + 64, 8);
    
    serial_puts("NVMe: Model: ");
    serial_puts(ctrl->model);
    serial_puts(", Serial: ");
    serial_puts(ctrl->serial);
    serial_puts(", Firmware: ");
    serial_puts(ctrl->firmware);
    serial_puts("\n");
    
    kfree(identify_data);
    return 0;
}

// 识别命名空间
int nvme_identify_namespace(struct nvme_controller *ctrl, uint32_t nsid) {
    if (!ctrl || nsid == 0) return -1;
    
    // 分配识别数据缓冲区
    uint8_t *identify_data = kmalloc(4096);
    if (!identify_data) return -1;
    
    // 构建识别命令
    struct nvme_sq_entry cmd = {0};
    cmd.cdw0 = (NVME_OPC_ADMIN_IDENTIFY << 8) | 2; // 命令ID
    cmd.nsid = nsid;
    cmd.prp1 = (uint64_t)identify_data;
    cmd.cdw10 = 0; // CNS=0 表示命名空间识别
    
    // 发送命令
    if (nvme_submit_admin_cmd(ctrl, &cmd) != 0) {
        kfree(identify_data);
        return -1;
    }
    
    // 解析命名空间数据
    struct nvme_namespace *ns = &ctrl->namespaces[ctrl->num_namespaces++];
    ns->nsid = nsid;
    ns->size = *((uint64_t*)(identify_data + 0));
    ns->block_size = 1 << *((uint32_t*)(identify_data + 128));
    ns->capacity = ns->size * ns->block_size;
    
    serial_puts("NVMe: Namespace ");
    serial_putdec32(nsid);
    serial_puts(": Size=");
    serial_putdec64(ns->size);
    serial_puts(" blocks, Block size=");
    serial_putdec32(ns->block_size);
    serial_puts(" bytes\n");
    
    kfree(identify_data);
    return 0;
}

// 创建IO队列
int nvme_create_io_queue(struct nvme_controller *ctrl, uint16_t qid, uint16_t vector) {
    if (!ctrl || qid >= NVME_MAX_QUEUES) return -1;
    
    struct nvme_queue_pair *qp = &ctrl->io_queues[qid];
    
    // 分配队列内存
    qp->sq = kmalloc(sizeof(struct nvme_sq_entry) * NVME_QUEUE_SIZE);
    qp->cq = kmalloc(sizeof(struct nvme_cq_entry) * NVME_QUEUE_SIZE);
    if (!qp->sq || !qp->cq) {
        if (qp->sq) kfree(qp->sq);
        if (qp->cq) kfree(qp->cq);
        return -1;
    }
    
    // 初始化队列
    qp->qid = qid;
    qp->vector = vector;
    qp->size = NVME_QUEUE_SIZE;
    qp->sq_tail = 0;
    qp->cq_head = 0;
    qp->phase = 1;
    qp->enabled = true;
    
    // 构建创建队列命令
    struct nvme_sq_entry cmd = {0};
    cmd.cdw0 = (NVME_OPC_ADMIN_CREATE_IOQ << 8) | (qid + 3);
    cmd.prp1 = (uint64_t)qp->sq; // 提交队列物理地址
    cmd.cdw10 = (qid << 16) | (NVME_QUEUE_SIZE - 1);
    cmd.cdw11 = (vector << 16) | (qid << 0); // 中断向量和队列ID
    cmd.cdw12 = 0x01; // 队列类型：提交队列
    
    if (nvme_submit_admin_cmd(ctrl, &cmd) != 0) {
        kfree(qp->sq);
        kfree(qp->cq);
        return -1;
    }
    
    // 创建完成队列
    cmd.cdw0 = (NVME_OPC_ADMIN_CREATE_IOQ << 8) | (qid + 4);
    cmd.prp1 = (uint64_t)qp->cq; // 完成队列物理地址
    cmd.cdw12 = 0x00; // 队列类型：完成队列
    
    if (nvme_submit_admin_cmd(ctrl, &cmd) != 0) {
        kfree(qp->sq);
        kfree(qp->cq);
        return -1;
    }
    
    ctrl->num_io_queues++;
    return 0;
}

// NVMe读操作
int nvme_read(struct nvme_controller *ctrl, uint32_t nsid, uint64_t lba, 
              uint32_t count, void *buffer, uint16_t qid) {
    if (!ctrl || !buffer || count == 0) return -1;
    
    struct nvme_queue_pair *qp = &ctrl->io_queues[qid];
    if (!qp->enabled) return -1;
    
    // 构建读命令
    struct nvme_sq_entry cmd = {0};
    cmd.cdw0 = (NVME_OPC_IO_READ << 8) | (qid + 10);
    cmd.nsid = nsid;
    cmd.prp1 = (uint64_t)buffer;
    cmd.cdw10 = (uint32_t)lba;
    cmd.cdw11 = (uint32_t)(lba >> 32);
    cmd.cdw12 = (count - 1) & 0xFFFF; // 传输长度
    
    return nvme_submit_io_cmd(ctrl, qp, &cmd);
}

// NVMe写操作
int nvme_write(struct nvme_controller *ctrl, uint32_t nsid, uint64_t lba, 
               uint32_t count, const void *buffer, uint16_t qid) {
    if (!ctrl || !buffer || count == 0) return -1;
    
    struct nvme_queue_pair *qp = &ctrl->io_queues[qid];
    if (!qp->enabled) return -1;
    
    // 构建写命令
    struct nvme_sq_entry cmd = {0};
    cmd.cdw0 = (NVME_OPC_IO_WRITE << 8) | (qid + 10);
    cmd.nsid = nsid;
    cmd.prp1 = (uint64_t)buffer;
    cmd.cdw10 = (uint32_t)lba;
    cmd.cdw11 = (uint32_t)(lba >> 32);
    cmd.cdw12 = (count - 1) & 0xFFFF; // 传输长度
    
    return nvme_submit_io_cmd(ctrl, qp, &cmd);
}

// 批量读操作
int nvme_read_batch(struct nvme_controller *ctrl, uint32_t nsid, 
                    uint64_t *lba_array, uint32_t *count_array, 
                    void **buffer_array, uint32_t cmd_count, uint16_t qid) {
    if (!ctrl || !lba_array || !count_array || !buffer_array || cmd_count == 0) {
        return -1;
    }
    
    struct nvme_queue_pair *qp = &ctrl->io_queues[qid];
    if (!qp->enabled) return -1;
    
    // 检查队列空间是否足够
    if ((qp->sq_tail + cmd_count) % qp->size <= qp->cq_head) {
        return -1; // 队列空间不足
    }
    
    // 提交批量命令
    for (uint32_t i = 0; i < cmd_count; i++) {
        struct nvme_sq_entry cmd = {0};
        cmd.cdw0 = (NVME_OPC_IO_READ << 8) | (qid + 10 + i);
        cmd.nsid = nsid;
        cmd.prp1 = (uint64_t)buffer_array[i];
        cmd.cdw10 = (uint32_t)lba_array[i];
        cmd.cdw11 = (uint32_t)(lba_array[i] >> 32);
        cmd.cdw12 = (count_array[i] - 1) & 0xFFFF;
        
        if (nvme_submit_io_cmd(ctrl, qp, &cmd) != 0) {
            return -1;
        }
    }
    
    return 0;
}

// 批量写操作
int nvme_write_batch(struct nvme_controller *ctrl, uint32_t nsid, 
                     uint64_t *lba_array, uint32_t *count_array, 
                     const void **buffer_array, uint32_t cmd_count, uint16_t qid) {
    if (!ctrl || !lba_array || !count_array || !buffer_array || cmd_count == 0) {
        return -1;
    }
    
    struct nvme_queue_pair *qp = &ctrl->io_queues[qid];
    if (!qp->enabled) return -1;
    
    // 检查队列空间是否足够
    if ((qp->sq_tail + cmd_count) % qp->size <= qp->cq_head) {
        return -1; // 队列空间不足
    }
    
    // 提交批量命令
    for (uint32_t i = 0; i < cmd_count; i++) {
        struct nvme_sq_entry cmd = {0};
        cmd.cdw0 = (NVME_OPC_IO_WRITE << 8) | (qid + 10 + i);
        cmd.nsid = nsid;
        cmd.prp1 = (uint64_t)buffer_array[i];
        cmd.cdw10 = (uint32_t)lba_array[i];
        cmd.cdw11 = (uint32_t)(lba_array[i] >> 32);
        cmd.cdw12 = (count_array[i] - 1) & 0xFFFF;
        
        if (nvme_submit_io_cmd(ctrl, qp, &cmd) != 0) {
            return -1;
        }
    }
    
    return 0;
}

// 中断处理函数
void nvme_irq_handler(uint32_t irq, void *data) {
    struct nvme_controller *ctrl = (struct nvme_controller*)data;
    if (!ctrl) return;
    
    // 处理所有IO队列的中断
    for (uint16_t qid = 0; qid < ctrl->num_io_queues; qid++) {
        struct nvme_queue_pair *qp = &ctrl->io_queues[qid];
        if (!qp->enabled) continue;
        
        // 处理完成队列条目
        while (true) {
            struct nvme_cq_entry *entry = &qp->cq[qp->cq_head];
            
            // 检查阶段位是否匹配
            if ((entry->status >> 16) != qp->phase) {
                break; // 没有更多完成条目
            }
            
            // 更新完成队列头指针
            qp->cq_head = (qp->cq_head + 1) % qp->size;
            if (qp->cq_head == 0) {
                qp->phase = !qp->phase; // 翻转阶段位
            }
            
            // 更新门铃寄存器
            *qp->cq_db = qp->cq_head;
        }
    }
}

// 设置MSI中断
int nvme_setup_msi(pci_device_t *dev) {
    // 简化实现，返回成功
    serial_puts("NVMe: MSI setup completed (simplified)\n");
    return 0;
}

// 设置电源状态
int nvme_set_power_state(struct nvme_controller *ctrl, uint8_t state) {
    if (!ctrl) return -1;
    
    // 构建设置电源状态命令
    struct nvme_sq_entry cmd = {0};
    cmd.cdw0 = (0x02 << 8) | 1; // 设置特性命令
    cmd.cdw10 = 0x02; // 电源管理特性
    cmd.cdw11 = state & 0x0F; // 电源状态
    
    return nvme_submit_admin_cmd(ctrl, &cmd);
}

// 获取性能统计
void nvme_get_perf_stats(struct nvme_controller *ctrl, 
                        uint64_t *read_ops, uint64_t *write_ops,
                        uint64_t *total_bytes, uint64_t *avg_latency) {
    if (!ctrl) return;
    
    // 在实际实现中，这里应该维护性能计数器
    // 这里返回示例数据
    if (read_ops) *read_ops = 0;
    if (write_ops) *write_ops = 0;
    if (total_bytes) *total_bytes = 0;
    if (avg_latency) *avg_latency = 0;
}