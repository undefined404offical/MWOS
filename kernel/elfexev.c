#include "elfexev.h"
#include "elf.h"
#include "klog.h"
#include "memory.h"
#include "pmm.h"
#include "serial.h"
#include "string.h"
#include "vmm.h"
#include "gdt.h"
#include "drivers/fs/vfs.h"

/* 调试输出 helper – 确保输出到串口 */
static void elf_printf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    serial_puts(buf);
}

/* ------------------------------------------------------------------ */
/* 全局变量 – 供 syscall 退出时跳回内核                                */
/* ------------------------------------------------------------------ */
uint64_t g_elf_ret_rip = 0;
uint64_t g_elf_ret_rsp = 0;

/* 退出回调 – 外部可注册（如 shell_print_prompt）                       */
void (*g_elf_on_exit)(void) = NULL;

/* ------------------------------------------------------------------ */
/* 退出处理 – 用户程序 sys_exit 后回到这里                            */
/* ------------------------------------------------------------------ */
static void __attribute__((noreturn)) elf_exit_handler(void)
{
    serial_puts("\n[program exited]\n");
    if (g_elf_on_exit)
        g_elf_on_exit();
    for (;;)
        asm volatile("hlt");
}

/* ------------------------------------------------------------------ */
/* 对齐到页边界                                                       */
/* ------------------------------------------------------------------ */
static inline uint64_t page_align_up(uint64_t addr)
{
    return (addr + 0xFFF) & ~0xFFFULL;
}

static inline uint64_t page_align_down(uint64_t addr)
{
    return addr & ~0xFFFULL;
}

/* ------------------------------------------------------------------ */
/* elfexec – 加载 ELF 文件并切换到用户态                               */
/*                                                                     */
/* 参数:                                                               */
/*   p    – 当前进程控制块（暂未使用，保留 API 兼容）                   */
/*   path – 文件路径                                                   */
/*   argv – 命令行参数（暂未使用）                                      */
/*   envp – 环境变量（暂未使用）                                        */
/*                                                                     */
/* 返回: 0=成功, -1=失败                                               */
/* ------------------------------------------------------------------ */
int elfexec(struct proc *p,
            const char *path,
            char *const argv[],
            char *const envp[])
{
    (void)p;
    (void)argv;
    (void)envp;

    uint8_t *file_buf = NULL;
    uint32_t file_size;
    int ret = -1;

    /* ---- 1. 从 VFS 读取整个 ELF 文件 ---- */
    vfs_file_t file;
    if (!vfs_open(path, &file, VFS_READ)) {
        elf_printf("[ELF] ERROR: Cannot open: %s\n", path);
        return -1;
    }

    if (file.is_directory) {
        elf_printf("[ELF] ERROR: '%s' is a directory\n", path);
        vfs_close(&file);
        return -1;
    }

    file_size = file.file_size;
    if (file_size < sizeof(Elf64_Ehdr)) {
        elf_printf("[ELF] ERROR: File too small: %u bytes\n", file_size);
        vfs_close(&file);
        return -1;
    }

    file_buf = (uint8_t *)kmalloc(file_size);
    if (!file_buf) {
        elf_printf("[ELF] ERROR: Out of memory (%u bytes)\n", file_size);
        vfs_close(&file);
        return -1;
    }

    {
        uint32_t total = 0, chunk, br;
        while (total < file_size) {
            chunk = file_size - total;
            if (chunk > 512) chunk = 512;
            if (!vfs_read(&file, file_buf + total, chunk, &br)) {
                elf_printf("[ELF] ERROR: Read error at offset %u\n", total);
                vfs_close(&file);
                kfree(file_buf);
                return -1;
            }
            if (br == 0) break;
            total += br;
        }
    }
    vfs_close(&file);

    /* ---- 2. 校验 ELF 头部 ---- */
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)file_buf;

    if (ehdr->e_ident[0] != ELFMAG0 ||
        ehdr->e_ident[1] != ELFMAG1 ||
        ehdr->e_ident[2] != ELFMAG2 ||
        ehdr->e_ident[3] != ELFMAG3) {
        elf_printf("[ELF] ERROR: Bad magic\n");
        goto fail;
    }

    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        elf_printf("[ELF] ERROR: Not 64-bit\n");
        goto fail;
    }

    if (ehdr->e_type != ET_EXEC) {
        elf_printf("[ELF] ERROR: Not an executable (type=%u)\n", ehdr->e_type);
        goto fail;
    }

    if (ehdr->e_machine != EM_X86_64) {
        elf_printf("[ELF] ERROR: Not x86_64 (machine=%u)\n", ehdr->e_machine);
        goto fail;
    }

    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) {
        elf_printf("[ELF] ERROR: No program headers\n");
        goto fail;
    }

    uint64_t entry = ehdr->e_entry;
    elf_printf("[ELF] entry=0x%lX, %u program headers\n",
               entry, ehdr->e_phnum);

    /* ---- 3. 遍历 program headers, 映射并复制每个 PT_LOAD ---- */
    Elf64_Phdr *phdr = (Elf64_Phdr *)(file_buf + ehdr->e_phoff);

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD)
            continue;

        uint64_t vaddr  = phdr[i].p_vaddr;
        uint64_t filesz = phdr[i].p_filesz;
        uint64_t memsz  = phdr[i].p_memsz;
        uint64_t offset = phdr[i].p_offset;
        uint32_t flags  = phdr[i].p_flags;

        uint64_t base   = page_align_down(vaddr);
        uint64_t top    = page_align_up(vaddr + memsz);
        uint64_t npages = (top - base) / PAGE_SIZE;

        elf_printf("[ELF]   LOAD [%u] vaddr=0x%lX size=%lu/%lu flags=%c%c%c\n",
              i, vaddr, filesz, memsz,
              (flags & PF_R) ? 'R' : '-',
              (flags & PF_W) ? 'W' : '-',
              (flags & PF_X) ? 'X' : '-');

        /* 为每个页分配物理页并映射为用户可访问 */
        /* 如果页已经映射为 USER 页（来自前面的 PT_LOAD 重叠），跳过 */
        for (uint64_t page = base; page < top; page += PAGE_SIZE) {
            bool already_user = false;
            /* 检查页是否已存在且是 USER 映射 */
            pt_entry_t pml4e = kernel_pml4[PML4_IDX(page)];
            if (pml4e & PTE_PRESENT) {
                pt_entry_t *pdpt = (pt_entry_t *)(pml4e & PTE_ADDR_MASK);
                pt_entry_t pdpte = pdpt[PDPT_IDX(page)];
                if (pdpte & PTE_PRESENT) {
                    pt_entry_t *pd = (pt_entry_t *)(pdpte & PTE_ADDR_MASK);
                    pt_entry_t pde = pd[PD_IDX(page)];
                    if (pde & PTE_PRESENT) {
                        pt_entry_t *pt = (pt_entry_t *)(pde & PTE_ADDR_MASK);
                        if ((pt[PT_IDX(page)] & (PTE_PRESENT | PTE_USER))
                            == (PTE_PRESENT | PTE_USER))
                            already_user = true;
                    }
                }
            }
            if (already_user)
                continue;   /* 已由前一个 PT_LOAD 映射，跳过 */

            /* 分配新页并映射为用户可访问 */
            void *phys = pmm_alloc_zpage();
            if (!phys) {
                elf_printf("[ELF] ERROR: Out of memory (page)\n");
                goto fail;
            }
            vmm_map_user(kernel_pml4, page, (uint64_t)phys);
            vmm_make_user(kernel_pml4, page);
        }

        /* 从文件拷贝数据 (file data) */
        if (filesz > 0) {
            uint64_t src_offset = offset;
            uint64_t dst_offset = vaddr - base;  /* 可能不对齐页 */
            uint8_t *dst = (uint8_t *)base;
            memcpy(dst + dst_offset, file_buf + src_offset, filesz);
        }

        /* .bss 部分已经在 pmm_alloc_zpage 时清零 */
        /* 但需要确保 memsz > filesz 部分确实为 0 */
        /* (pmm_alloc_zpage 已经清零整个页) */
    }

    /* ---- 4. 分配用户栈 ---- */
    uint64_t stack_vaddr = 0x600000;  /* 用户栈基址 */
    uint64_t stack_pages = 4;         /* 16 KB 栈 */
    uint64_t stack_rsp   = stack_vaddr + stack_pages * PAGE_SIZE;

    for (uint64_t page = stack_vaddr; page < stack_rsp; page += PAGE_SIZE) {
        void *phys = pmm_alloc_zpage();
        if (!phys) {
            elf_printf("[ELF] ERROR: Out of memory (stack)\n");
            goto fail;
        }
        vmm_map_user(kernel_pml4, page, (uint64_t)phys);
        vmm_make_user(kernel_pml4, page);
    }

    elf_printf("[ELF]   stack @ 0x%lX - 0x%lX\n", stack_vaddr, stack_rsp);

    /* ---- 5. 保存内核返回信息 ---- */
    uint64_t kernel_rsp;
    asm volatile("mov %%rsp, %0" : "=r"(kernel_rsp));
    g_elf_ret_rsp = kernel_rsp;
    g_elf_ret_rip = (uint64_t)elf_exit_handler;

    /* 更新 TSS.RSP0，使 ring 3 的 int/syscall 回到正确的内核栈 */
    tss_set_stack(kernel_rsp + 512);  /* 留出足够空间给中断帧 */

    /* ---- 6. 进入用户态 ---- */
    serial_puts("[ELF] entering user mode...\n");
    enter_usermode(entry, stack_rsp);

    /* ---- 用户程序退出后回到这里 ---- */
    elf_exit_handler();
    kfree(file_buf);
    return 0;

fail:
    if (file_buf)
        kfree(file_buf);
    return -1;
}
