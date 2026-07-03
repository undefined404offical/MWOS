#include "vmm.h"
#include "kernel.h"
#include "memory.h" // pmm_alloc_zpage
#include "serial.h"
#include <stdint.h>

extern boot_params_t kernel_params;

// 全局的内核 PML4 指针，给 kmain.c / 高半区映射用
pt_entry_t *kernel_pml4 = NULL;

// =======================
// Helpers
// =======================

// 获取下一级页表，如果不存在则分配
static pt_entry_t *get_next_level(pt_entry_t *entry, uint64_t flags)
{
    if (*entry & PTE_PRESENT)
    {
        return (pt_entry_t *)(*entry & PTE_ADDR_MASK);
    }

    void *new_table = pmm_alloc_zpage();
    if (!new_table)
    {
        serial_puts("vmm: pmm_alloc_zpage failed\n");
        return NULL;
    }

    *entry = (uint64_t)new_table | PTE_PRESENT | flags;
    return (pt_entry_t *)new_table;
}

// 映射单个 4KB 页：virt → phys
static void vmm_map(pt_entry_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags)
{
    pt_entry_t *pdpt = get_next_level(&pml4[PML4_IDX(virt)], flags);
    if (!pdpt)
        return;

    pt_entry_t *pd = get_next_level(&pdpt[PDPT_IDX(virt)], flags);
    if (!pd)
        return;

    pt_entry_t *pt = get_next_level(&pd[PD_IDX(virt)], flags);
    if (!pt)
        return;

    pt[PT_IDX(virt)] = (phys & PTE_ADDR_MASK) | flags | PTE_PRESENT;

    // 刷新 TLB
    asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

// mark an existing page as user-accessible (or-in USER bit)
void vmm_make_user(pt_entry_t *pml4, uint64_t virt)
{
    pt_entry_t *pdpt = (pt_entry_t*)(pml4[PML4_IDX(virt)] & PTE_ADDR_MASK);
    if (!pdpt) return;
    pml4[PML4_IDX(virt)] |= PTE_USER;

    pt_entry_t *pd = (pt_entry_t*)(pdpt[PDPT_IDX(virt)] & PTE_ADDR_MASK);
    if (!pd) return;
    pdpt[PDPT_IDX(virt)] |= PTE_USER;

    pt_entry_t *pt = (pt_entry_t*)(pd[PD_IDX(virt)] & PTE_ADDR_MASK);
    if (!pt) return;
    pd[PD_IDX(virt)] |= PTE_USER;

    pt[PT_IDX(virt)] |= PTE_USER;
    asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

// kernel mapping (U/S=0)
void vmm_map_kernel(pt_entry_t *pml4, uint64_t virt, uint64_t phys)
{
    uint64_t flags = PTE_WRITABLE;
    vmm_map(pml4, virt, phys, flags);
}

// user mapping (U/S=1, writable)
void vmm_map_user(pt_entry_t *pml4, uint64_t virt, uint64_t phys)
{
    uint64_t flags = PTE_WRITABLE | PTE_USER;
    vmm_map(pml4, virt, phys, flags);
}

// 切换当前 PML4
static void vmm_switch_table(pt_entry_t *pml4)
{
    asm volatile("mov %0, %%cr3" : : "r"(pml4) : "memory");
}

// =======================
// Public API
// =======================

void vmm_init(void)
{
    serial_puts("vmm: initializing page tables...\n");

    // 分配新的 PML4 —— 注意这里用全局变量，不要再定义同名局部变量
    kernel_pml4 = (pt_entry_t *)pmm_alloc_zpage();
    if (!kernel_pml4)
    {
        serial_puts("vmm: FATAL: cannot allocate PML4\n");
        while (1)
        {
        }
    }

    uint64_t flags = PTE_WRITABLE; // 内核态 R/W

    // ------------------------------------------------------------
    // 1) 标识映射前 1GB
    // ------------------------------------------------------------
    serial_puts("vmm: identity-map 0–1GB\n");

    for (uint64_t addr = 0; addr < 0x40000000ULL; addr += PAGE_SIZE)
    {
        vmm_map(kernel_pml4, addr, addr, flags);
    }

    // ------------------------------------------------------------
    // 2) 显存区域标识映射
    // ------------------------------------------------------------
    uint64_t fb_base = kernel_params.framebuffer_addr;
    uint64_t fb_size = kernel_params.framebuffer_size;

    if (fb_size != 0)
    {
        serial_puts("vmm: identity-map framebuffer: base=0x");
        serial_puthex64(fb_base);
        serial_puts(" size=0x");
        serial_puthex64(fb_size);
        serial_puts("\n");

        uint64_t fb_start = fb_base & ~(PAGE_SIZE - 1);
        uint64_t fb_end = (fb_base + fb_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

        for (uint64_t addr = fb_start; addr < fb_end; addr += PAGE_SIZE)
        {
            vmm_map(kernel_pml4, addr, addr, flags);
        }
    }

    // ------------------------------------------------------------
    // 3) PCI MMIO 区域（低地址）
    // ------------------------------------------------------------
    uint64_t mmio_start = 0xC0000000ULL;
    uint64_t mmio_end = 0xC2000000ULL;

    serial_puts("vmm: identity-map PCI MMIO: [0x");
    serial_puthex64(mmio_start);
    serial_puts(", 0x");
    serial_puthex64(mmio_end);
    serial_puts(")\n");

    for (uint64_t addr = mmio_start; addr < mmio_end; addr += PAGE_SIZE)
    {
        vmm_map(kernel_pml4, addr, addr, flags);
    }

    // ------------------------------------------------------------
    // 4) 高地址 PCI MMIO（例如 HDA BAR0）
    // ------------------------------------------------------------
    uint64_t mmio_hi_start = 0x80000000ULL;
    uint64_t mmio_hi_end = 0x82000000ULL;

    serial_puts("vmm: identity-map high PCI MMIO: [0x");
    serial_puthex64(mmio_hi_start);
    serial_puts(", 0x");
    serial_puthex64(mmio_hi_end);
    serial_puts(")\n");

    for (uint64_t addr = mmio_hi_start; addr < mmio_hi_end; addr += PAGE_SIZE)
    {
        vmm_map(kernel_pml4, addr, addr, flags);
    }

    // ------------------------------------------------------------
    // 切换到新的页表
    // ------------------------------------------------------------
    vmm_switch_table(kernel_pml4);

    serial_puts("vmm: paging enabled with new PML4\n");
}
