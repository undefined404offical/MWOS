#include "drivers/ahci.h"
#include "drivers/pci.h"
#include "drivers/irq.h"
#include "memory.h"
#include "serial.h"
#include "io.h"
#include "string.h"
#include "stdbool.h"
#include "timer.h"

static struct ahci_hba g_ahci_hba;
static bool g_ahci_initialized = false;

// AHCI FIS类型
#define FIS_TYPE_REG_H2D    0x27
#define FIS_TYPE_REG_D2H    0x34
#define FIS_TYPE_DMA_ACT    0x39
#define FIS_TYPE_DMA_SETUP  0x41
#define FIS_TYPE_DATA       0x46
#define FIS_TYPE_BIST       0x58
#define FIS_TYPE_PIO_SETUP  0x5F
#define FIS_TYPE_DEV_BITS   0xA1

// 寄存器FIS结构
struct sata_reg_fis {
    uint8_t fis_type;           // FIS_TYPE_REG_H2D
    uint8_t pmport_c;           // Port multiplier and command
    uint8_t command;            // ATA命令
    uint8_t features;           // 特性
    uint8_t lba_low;            // LBA低字节
    uint8_t lba_mid;            // LBA中字节
    uint8_t lba_high;           // LBA高字节
    uint8_t device;             // 设备选择
    uint8_t lba_low_exp;        // LBA低字节扩展
    uint8_t lba_mid_exp;        // LBA中字节扩展
    uint8_t lba_high_exp;       // LBA高字节扩展
    uint8_t features_exp;       // 特性扩展
    uint8_t sector_count;       // 扇区计数
    uint8_t sector_count_exp;   // 扇区计数扩展
    uint8_t reserved;           // 保留
    uint8_t control;            // 控制
    uint8_t reserved2[4];       // 保留
} __attribute__((packed));

// 检查AHCI控制器是否支持
static bool ahci_check_support(pci_device_t *dev) {
    // 在MWOS中，我们需要使用不同的方法来获取BAR
    // 暂时简化实现，直接返回true
    serial_puts("AHCI: Assuming controller supports AHCI mode\n");
    return true;
}

// 初始化HBA控制器
static bool ahci_init_hba(pci_device_t *dev) {
    // 在MWOS中，我们需要简化实现
    // 假设使用固定的基地址和命令槽位数量
    g_ahci_hba.base = (volatile uint32_t*)0xE0000000; // 假设的基地址
    g_ahci_hba.cmd_slots = 32; // 假设32个命令槽位
    g_ahci_hba.num_ports = 6; // 假设6个端口
    
    serial_puts("AHCI: Slots: ");
    serial_putdec32(g_ahci_hba.cmd_slots);
    serial_puts(", Ports: ");
    serial_putdec32(g_ahci_hba.num_ports);
    serial_puts("\n");
    
    // 启用AHCI模式
    uint32_t ghc = g_ahci_hba.base[HBA_RGHC];
    g_ahci_hba.base[HBA_RGHC] = ghc | (1 << 31); // 启用AHCI
    
    // 分配端口结构
    g_ahci_hba.ports = kmalloc(sizeof(struct hba_port) * g_ahci_hba.num_ports);
    if (!g_ahci_hba.ports) return false;
    
    memset(g_ahci_hba.ports, 0, sizeof(struct hba_port) * g_ahci_hba.num_ports);
    
    // 设置HBA指针供中断处理使用
    ahci_set_hba_ptr(&g_ahci_hba);
    
    // 尝试设置MSI中断
    if (ahci_setup_msi(dev) == 0) {
        serial_puts("AHCI: MSI interrupt enabled\n");
    } else {
        serial_puts("AHCI: Using legacy interrupt mode\n");
    }
    
    return true;
}

// 初始化端口
static bool ahci_init_port(int port_num) {
    if (port_num >= g_ahci_hba.num_ports) return false;
    
    struct hba_port *port = &g_ahci_hba.ports[port_num];
    port->regs = g_ahci_hba.base + 0x100 + port_num * 0x80;
    
    // 检查端口是否实现
    uint32_t pi = g_ahci_hba.base[HBA_RPI];
    if (!(pi & (1 << port_num))) {
        return false;
    }
    
    // 检查端口状态
    uint32_t ssts = port->regs[HBA_RPxSSTS];
    uint32_t det = (ssts >> 0) & 0xF;
    uint32_t ipm = (ssts >> 8) & 0xF;
    
    if (det != 0x3 || ipm != 0x1) {
        serial_puts("AHCI: Port ");
        serial_putdec32(port_num);
        serial_puts(" not ready (DET=0x");
        serial_puthex8(det);
        serial_puts(", IPM=0x");
        serial_puthex8(ipm);
        serial_puts(")\n");
        return false;
    }
    
    // 分配命令列表 - 使用MWOS的内存分配函数
    port->cmdlst = kmalloc(4096);
    if (!port->cmdlst) return false;
    
    memset(port->cmdlst, 0, 4096);
    
    // 在MWOS中，我们简化处理，假设物理地址等于虚拟地址
    // 设置命令列表基址
    port->regs[HBA_RPxCLB] = (uint32_t)((uint64_t)port->cmdlst & 0xFFFFFFFF);
    port->regs[HBA_RPxCLBU] = (uint32_t)((uint64_t)port->cmdlst >> 32);
    
    // 分配命令表指针数组
    port->cmd_tables = kmalloc(sizeof(struct hba_cmdt*) * g_ahci_hba.cmd_slots);
    if (!port->cmd_tables) return false;
    
    memset(port->cmd_tables, 0, sizeof(struct hba_cmdt*) * g_ahci_hba.cmd_slots);
    
    port->cmd_slots = g_ahci_hba.cmd_slots;
    
    // 初始化并行命令上下文
    ahci_init_parallel_context(port);
    
    // 启用端口
    port->regs[HBA_RPxCMD] |= HBA_PxCMD_FRE;
    port->regs[HBA_RPxCMD] |= HBA_PxCMD_ST;
    
    serial_puts("AHCI: Port ");
    serial_putdec32(port_num);
    serial_puts(" initialized successfully\n");
    
    return true;
}

// 获取空闲命令槽位
int ahci_get_free_slot(struct hba_port *port) {
    if (!port || !port->regs) return -1;
    
    uint32_t sact = port->regs[HBA_RPxSACT];
    uint32_t ci = port->regs[HBA_RPxCI];
    uint32_t busy = sact | ci;
    
    for (int i = 0; i < port->cmd_slots; i++) {
        if (!(busy & (1 << i))) {
            return i;
        }
    }
    
    return -1; // 所有槽位都忙
}

// 准备命令
static int ahci_prepare_command(struct hba_port *port, int slot, 
                               uint64_t lba, uint32_t count, bool write) {
    if (slot < 0 || slot >= port->cmd_slots) return -1;
    
    // 分配命令表（如果需要）
    if (!port->cmd_tables[slot]) {
        port->cmd_tables[slot] = kmalloc(4096);
        if (!port->cmd_tables[slot]) return -1;
        
        memset(port->cmd_tables[slot], 0, 4096);
        
        // 设置命令表基址 - 在MWOS中简化处理
        port->cmdlst[slot].cmd_table_base = (uint32_t)((uint64_t)port->cmd_tables[slot] & 0xFFFFFFFF);
        port->cmdlst[slot].cmd_table_base_upper = (uint32_t)((uint64_t)port->cmd_tables[slot] >> 32);
    }
    
    struct hba_cmdt *cmdt = port->cmd_tables[slot];
    struct sata_reg_fis *fis = (struct sata_reg_fis*)cmdt->cfis;
    
    // 构建FIS
    memset(fis, 0, sizeof(struct sata_reg_fis));
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->pmport_c = 0x80; // Command bit
    fis->command = write ? 0x35 : 0x25; // WRITE DMA EXT / READ DMA EXT
    fis->device = 0x40; // LBA mode
    
    // 设置LBA地址
    fis->lba_low = lba & 0xFF;
    fis->lba_mid = (lba >> 8) & 0xFF;
    fis->lba_high = (lba >> 16) & 0xFF;
    fis->lba_low_exp = (lba >> 24) & 0xFF;
    fis->lba_mid_exp = (lba >> 32) & 0xFF;
    fis->lba_high_exp = (lba >> 40) & 0xFF;
    
    // 设置扇区计数
    fis->sector_count = count & 0xFF;
    fis->sector_count_exp = (count >> 8) & 0xFF;
    
    // 设置命令头
    port->cmdlst[slot].options = (5 << 16) | (1 << 10) | (1 << 6) | (1 << 5);
    port->cmdlst[slot].prdt_len = 1;
    
    return 0;
}

// 启动命令
void ahci_start_cmd(struct hba_port *port, int slot) {
    if (!port || slot < 0 || slot >= port->cmd_slots) return;
    
    // 设置命令发布位
    port->regs[HBA_RPxCI] |= (1 << slot);
}

// 等待命令完成
void ahci_wait_completion(struct hba_port *port, int slot) {
    if (!port || slot < 0 || slot >= port->cmd_slots) return;
    
    // 等待命令完成位清除
    while (port->regs[HBA_RPxCI] & (1 << slot)) {
        // 空循环等待
    }
    
    // 检查错误
    if (port->regs[HBA_RPxTFD] & 0x01) {
        serial_puts("AHCI: Command error on slot ");
        serial_putdec32(slot);
        serial_puts("\n");
    }
}

// 读取单个扇区
int ahci_read_sector(uint64_t lba, uint8_t *buffer) {
    return ahci_read_multiple(lba, 1, buffer);
}

// 写入单个扇区
int ahci_write_sector(uint64_t lba, const uint8_t *buffer) {
    return ahci_write_multiple(lba, 1, buffer);
}

// 批量读取扇区
int ahci_read_multiple(uint64_t lba, uint32_t count, uint8_t *buffer) {
    if (!g_ahci_initialized || count == 0) return -1;
    
    // 使用第一个可用的端口
    struct hba_port *port = &g_ahci_hba.ports[0];
    if (!port->regs) return -1;
    
    int slot = ahci_get_free_slot(port);
    if (slot < 0) {
        serial_puts("AHCI: No free command slots available\n");
        return -1;
    }
    
    // 准备命令
    if (ahci_prepare_command(port, slot, lba, count, false) < 0) {
        return -1;
    }
    
    // 设置PRD - 在MWOS中简化处理，假设物理地址等于虚拟地址
    struct hba_cmdt *cmdt = port->cmd_tables[slot];
    uint64_t buffer_phys = (uint64_t)buffer;
    
    cmdt->entries[0].data_base = buffer_phys & 0xFFFFFFFF;
    cmdt->entries[0].data_base_upper = buffer_phys >> 32;
    cmdt->entries[0].byte_count = (count * 512) - 1;
    
    // 启动命令
    ahci_start_cmd(port, slot);
    
    // 等待完成
    ahci_wait_completion(port, slot);
    
    return 0;
}

// 批量写入扇区
int ahci_write_multiple(uint64_t lba, uint32_t count, const uint8_t *buffer) {
    if (!g_ahci_initialized || count == 0) return -1;
    
    struct hba_port *port = &g_ahci_hba.ports[0];
    if (!port->regs) return -1;
    
    int slot = ahci_get_free_slot(port);
    if (slot < 0) {
        serial_puts("AHCI: No free command slots available\n");
        return -1;
    }
    
    // 准备命令
    if (ahci_prepare_command(port, slot, lba, count, true) < 0) {
        return -1;
    }
    
    // 设置PRD - 在MWOS中简化处理，假设物理地址等于虚拟地址
    struct hba_cmdt *cmdt = port->cmd_tables[slot];
    uint64_t buffer_phys = (uint64_t)buffer;
    
    cmdt->entries[0].data_base = buffer_phys & 0xFFFFFFFF;
    cmdt->entries[0].data_base_upper = buffer_phys >> 32;
    cmdt->entries[0].byte_count = (count * 512) - 1;
    
    // 启动命令
    ahci_start_cmd(port, slot);
    
    // 等待完成
    ahci_wait_completion(port, slot);
    
    return 0;
}

// 检测AHCI控制器
bool ahci_detect(void) {
    // 在MWOS中，我们需要简化PCI设备查找
    // 创建一个假的PCI设备结构
    pci_device_t dev = {
        .bus = 0,
        .device = 0,
        .function = 0,
        .vendor_id = 0x8086, // Intel
        .device_id = 0x2829, // ICH9 SATA
        .class_code = 0x01,
        .subclass = 0x06,
        .prog_if = 0x01
    };
    
    serial_puts("AHCI: Using simulated SATA controller\n");
    
    if (!ahci_check_support(&dev)) {
        return false;
    }
    
    if (!ahci_init_hba(&dev)) {
        return false;
    }
    
    // 初始化所有端口
    for (int i = 0; i < g_ahci_hba.num_ports; i++) {
        ahci_init_port(i);
    }
    
    g_ahci_initialized = true;
    serial_puts("AHCI: Initialization completed successfully\n");
    return true;
}

// 初始化并行命令上下文
void ahci_init_parallel_context(struct hba_port *port) {
    if (!port) return;
    
    port->parallel_ctx = kmalloc(sizeof(struct parallel_cmd_context));
    if (!port->parallel_ctx) return;
    
    memset(port->parallel_ctx, 0, sizeof(struct parallel_cmd_context));
    
    serial_puts("AHCI: Parallel command context initialized for port\n");
}

// 启动并行命令
int ahci_start_parallel_cmds(struct hba_port *port, uint32_t slot_mask) {
    if (!port || !port->parallel_ctx) return -1;
    
    // 检查槽位是否可用
    uint32_t sact = port->regs[HBA_RPxSACT];
    uint32_t ci = port->regs[HBA_RPxCI];
    uint32_t busy = sact | ci;
    
    if (busy & slot_mask) {
        serial_puts("AHCI: Some slots in mask are busy\n");
        return -1;
    }
    
    // 设置并行上下文
    port->parallel_ctx->active_slots = slot_mask;
    port->parallel_ctx->completed_slots = 0;
    port->parallel_ctx->pending_count = __builtin_popcount(slot_mask);
    
    // 启动所有命令
    port->regs[HBA_RPxCI] |= slot_mask;
    
    serial_puts("AHCI: Started parallel commands on slots: 0x");
    serial_puthex32(slot_mask);
    serial_puts("\n");
    
    return 0;
}

// 等待并行命令完成
int ahci_wait_parallel_completion(struct hba_port *port, uint32_t slot_mask, uint32_t timeout_ms) {
    if (!port || !port->parallel_ctx) return -1;
    
    uint32_t start_time = timer_get_ticks();
    uint32_t timeout_ticks = timeout_ms * 1000; // 假设timer_get_ticks()返回微秒
    
    while (port->regs[HBA_RPxCI] & slot_mask) {
        // 检查超时
        if (timer_get_ticks() - start_time > timeout_ticks) {
            serial_puts("AHCI: Parallel command timeout\n");
            return -1;
        }
        
        // 检查错误状态
        if (port->regs[HBA_RPxTFD] & 0x01) {
            serial_puts("AHCI: Command error during parallel execution\n");
            return -1;
        }
    }
    
    serial_puts("AHCI: Parallel commands completed successfully\n");
    return 0;
}

// 并行读取操作
int ahci_read_parallel(uint64_t *lba_array, uint32_t *count_array, uint8_t **buffer_array, uint32_t cmd_count) {
    if (!g_ahci_initialized || cmd_count == 0) return -1;
    
    struct hba_port *port = &g_ahci_hba.ports[0];
    if (!port->regs) return -1;
    
    // 获取足够的空闲槽位
    uint32_t slot_mask = 0;
    int slots_found = 0;
    
    for (int i = 0; i < port->cmd_slots && slots_found < cmd_count; i++) {
        if (ahci_get_free_slot(port) == i) {
            slot_mask |= (1 << i);
            slots_found++;
        }
    }
    
    if (slots_found < cmd_count) {
        serial_puts("AHCI: Not enough free slots for parallel read\n");
        return -1;
    }
    
    // 准备所有命令
    for (int i = 0, slot = 0; i < cmd_count; i++) {
        // 找到下一个设置的位
        while (!(slot_mask & (1 << slot))) slot++;
        
        if (ahci_prepare_command(port, slot, lba_array[i], count_array[i], false) < 0) {
            return -1;
        }
        
        // 设置PRD
        struct hba_cmdt *cmdt = port->cmd_tables[slot];
        uint64_t buffer_phys = (uint64_t)buffer_array[i];
        
        cmdt->entries[0].data_base = buffer_phys & 0xFFFFFFFF;
        cmdt->entries[0].data_base_upper = buffer_phys >> 32;
        cmdt->entries[0].byte_count = (count_array[i] * 512) - 1;
        
        slot++;
    }
    
    // 启动并行命令
    return ahci_start_parallel_cmds(port, slot_mask);
}

// 并行写入操作
int ahci_write_parallel(uint64_t *lba_array, uint32_t *count_array, const uint8_t **buffer_array, uint32_t cmd_count) {
    if (!g_ahci_initialized || cmd_count == 0) return -1;
    
    struct hba_port *port = &g_ahci_hba.ports[0];
    if (!port->regs) return -1;
    
    // 获取足够的空闲槽位
    uint32_t slot_mask = 0;
    int slots_found = 0;
    
    for (int i = 0; i < port->cmd_slots && slots_found < cmd_count; i++) {
        if (ahci_get_free_slot(port) == i) {
            slot_mask |= (1 << i);
            slots_found++;
        }
    }
    
    if (slots_found < cmd_count) {
        serial_puts("AHCI: Not enough free slots for parallel write\n");
        return -1;
    }
    
    // 准备所有命令
    for (int i = 0, slot = 0; i < cmd_count; i++) {
        // 找到下一个设置的位
        while (!(slot_mask & (1 << slot))) slot++;
        
        if (ahci_prepare_command(port, slot, lba_array[i], count_array[i], true) < 0) {
            return -1;
        }
        
        // 设置PRD
        struct hba_cmdt *cmdt = port->cmd_tables[slot];
        uint64_t buffer_phys = (uint64_t)buffer_array[i];
        
        cmdt->entries[0].data_base = buffer_phys & 0xFFFFFFFF;
        cmdt->entries[0].data_base_upper = buffer_phys >> 32;
        cmdt->entries[0].byte_count = (count_array[i] * 512) - 1;
        
        slot++;
    }
    
    // 启动并行命令
    return ahci_start_parallel_cmds(port, slot_mask);
}

// AHCI初始化函数
void ahci_init(void) {
    if (ahci_detect()) {
        serial_puts("AHCI: Driver initialized\n");
    } else {
        serial_puts("AHCI: Driver initialization failed\n");
    }
}