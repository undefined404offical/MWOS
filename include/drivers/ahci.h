#ifndef _AHCI_H
#define _AHCI_H

#include "stdint.h"
#include "stdbool.h"

// AHCI寄存器定义
#define HBA_PxCMD_ST    0x0001
#define HBA_PxCMD_FRE   0x0010
#define HBA_PxCMD_CR    0x8000
#define HBA_PxCMD_FR    0x4000

#define HBA_PxIS_TFES   (1 << 30)
#define HBA_PxIS_HBFS   (1 << 29)
#define HBA_PxIS_HBDS   (1 << 28)
#define HBA_PxIS_IFS    (1 << 27)
#define HBA_PxIS_INFS   (1 << 26)
#define HBA_PxIS_OFS    (1 << 24)
#define HBA_PxIS_IPMS   (1 << 23)
#define HBA_PxIS_PRCS   (1 << 22)
#define HBA_PxIS_DIAGS  (1 << 7)
#define HBA_PxIS_PCS    (1 << 6)
#define HBA_PxIS_DPS    (1 << 5)
#define HBA_PxIS_UFS    (1 << 4)
#define HBA_PxIS_SDBS   (1 << 3)
#define HBA_PxIS_DSS    (1 << 2)
#define HBA_PxIS_PSS    (1 << 1)
#define HBA_PxIS_DHRS   (1 << 0)

// HBA寄存器偏移
#define HBA_RCAP        0x00
#define HBA_RGHC        0x04
#define HBA_RIS         0x08
#define HBA_RPI         0x0C
#define HBA_RVS         0x10
#define HBA_RCCC        0x14
#define HBA_RCCP        0x18
#define HBA_REMCTL      0x2C
#define HBA_RVENDOR     0x70

// 端口寄存器偏移
#define HBA_RPxCLB      0x00
#define HBA_RPxCLBU     0x04
#define HBA_RPxFB       0x08
#define HBA_RPxFBU      0x0C
#define HBA_RPxIS       0x10
#define HBA_RPxIE       0x14
#define HBA_RPxCMD      0x18
#define HBA_RPxTFD      0x20
#define HBA_RPxSIG      0x24
#define HBA_RPxSSTS     0x28
#define HBA_RPxSCTL     0x2C
#define HBA_RPxSERR     0x30
#define HBA_RPxSACT     0x34
#define HBA_RPxCI       0x38
#define HBA_RPxSNTF     0x3C
#define HBA_RPxFBS      0x40
#define HBA_RPxDEVSLP   0x44
#define HBA_RPxVS       0x70

// 命令头结构
struct hba_cmdh {
    uint32_t options;           // DW0
    uint32_t prdt_len;          // DW1: PRDT长度
    uint32_t cmd_table_base;    // DW2: 命令表基址低32位
    uint32_t cmd_table_base_upper; // DW3: 命令表基址高32位
    uint32_t reserved[4];       // DW4-DW7
} __attribute__((packed));

// PRD条目结构
struct hba_prdte {
    uint32_t data_base;         // 数据基址低32位
    uint32_t data_base_upper;   // 数据基址高32位
    uint32_t reserved;          // 保留
    uint32_t byte_count;        // 字节计数(0-based)
} __attribute__((packed));

// 命令表结构
struct hba_cmdt {
    uint8_t cfis[64];           // FIS
    uint8_t acmd[16];            // ATAPI命令
    uint8_t reserved[48];        // 保留
    struct hba_prdte entries[]; // PRD条目数组
} __attribute__((packed));

// 并行命令上下文结构
struct parallel_cmd_context {
    uint32_t active_slots;       // 活动槽位位图
    uint32_t completed_slots;    // 完成槽位位图
    uint64_t start_time;         // 命令开始时间
    uint32_t pending_count;      // 待处理命令数
    void *user_data;             // 用户数据
};

// HBA端口结构
struct hba_port {
    volatile uint32_t *regs;     // 端口寄存器
    struct hba_cmdh *cmdlst;     // 命令列表
    struct hba_cmdt **cmd_tables; // 命令表指针数组
    uint32_t cmd_slots;          // 命令槽位数
    uint32_t sata_active;        // SATA活动状态
    uint32_t *completed;         // 完成位图
    struct parallel_cmd_context *parallel_ctx; // 并行命令上下文
};

// HBA控制器结构
struct ahci_hba {
    volatile uint32_t *base;     // HBA寄存器基址
    uint32_t num_ports;          // 端口数量
    struct hba_port *ports;     // 端口数组
    uint32_t cmd_slots;          // 命令槽位数
};

// 函数声明
void ahci_init(void);
bool ahci_detect(void);
int ahci_read_sector(uint64_t lba, uint8_t *buffer);
int ahci_write_sector(uint64_t lba, const uint8_t *buffer);
int ahci_read_multiple(uint64_t lba, uint32_t count, uint8_t *buffer);
int ahci_write_multiple(uint64_t lba, uint32_t count, const uint8_t *buffer);

// 中断处理相关
void ahci_set_hba_ptr(struct ahci_hba *hba);

// 命令队列管理
int ahci_get_free_slot(struct hba_port *port);
void ahci_start_cmd(struct hba_port *port, int slot);
void ahci_wait_completion(struct hba_port *port, int slot);

// 并行命令处理
int ahci_start_parallel_cmds(struct hba_port *port, uint32_t slot_mask);
int ahci_wait_parallel_completion(struct hba_port *port, uint32_t slot_mask, uint32_t timeout_ms);
int ahci_read_parallel(uint64_t *lba_array, uint32_t *count_array, uint8_t **buffer_array, uint32_t cmd_count);
int ahci_write_parallel(uint64_t *lba_array, uint32_t *count_array, const uint8_t **buffer_array, uint32_t cmd_count);
void ahci_init_parallel_context(struct hba_port *port);

#endif