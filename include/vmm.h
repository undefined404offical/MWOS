#ifndef VMM_H
#define VMM_H

#include <stdint.h>

#define PAGE_SIZE 4096ULL

// PTE flags
#define PTE_PRESENT   (1ULL << 0)
#define PTE_WRITABLE  (1ULL << 1)
#define PTE_USER      (1ULL << 2)
#define PTE_PWT       (1ULL << 3)
#define PTE_PCD       (1ULL << 4)
#define PTE_ACCESSED  (1ULL << 5)
#define PTE_DIRTY     (1ULL << 6)
#define PTE_PS        (1ULL << 7)
#define PTE_GLOBAL    (1ULL << 8)
#define PTE_NX        (1ULL << 63)

#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

// 索引宏
#define PML4_IDX(addr) (((addr) >> 39) & 0x1FF)
#define PDPT_IDX(addr) (((addr) >> 30) & 0x1FF)
#define PD_IDX(addr)   (((addr) >> 21) & 0x1FF)
#define PT_IDX(addr)   (((addr) >> 12) & 0x1FF)

// =======================
// 关键：类型必须放在最前面
// =======================
typedef uint64_t pt_entry_t;

// =======================
// 全局变量 / API
// =======================
extern pt_entry_t *kernel_pml4;

void vmm_map_kernel(pt_entry_t* pml4, uint64_t virt, uint64_t phys);
void vmm_map_user(pt_entry_t* pml4, uint64_t virt, uint64_t phys);
void vmm_make_user(pt_entry_t* pml4, uint64_t virt);
void vmm_init(void);

#endif // VMM_H
