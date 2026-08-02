// 出错先看是不是对齐问题！！！

#include "boot_splash.h"
#include "drivers/disk.h"
#include "drivers/fs/fscache.h"
#include "drivers/fs/vfs.h"
#include "drivers/fs/ext2.h"
#include "drivers/pci.h"
#include "font_manager.h"
#include "gdt.h"
#include "idt.h"
#include "graphics.h"
#include "kernel.h"
#include "klog.h"
#include "memory.h"
#include "pmm.h"
#include "shell.h"
#include "stdint.h"
#include "string.h"
#include "terminal.h"
#include "thread.h"
#include "ttf.h"
#include "ui/microui.h"
#include "vmm.h"
#include "elfexev.h"
#include "wm.h"



TTF_Font* g_font = NULL;

int32_t mouse_x = 0;
int32_t mouse_y = 0;
extern int32_t g_old_mouse_x;
extern int32_t g_old_mouse_y;

uint32_t screen_width;
uint32_t screen_height;

boot_params_t kernel_params;

TTF_Font* g_klog_font = NULL;

#define KERNEL_VIRT_BASE 0xFFFFFFFF80000000ULL

extern uint8_t _kernel_phys_start;
extern uint8_t _kernel_phys_end;

extern pt_entry_t* kernel_pml4;

static void map_kernel_high_half(void) {
    uint64_t k_phys_start = (uint64_t)&_kernel_phys_start;
    uint64_t k_phys_end = (uint64_t)&_kernel_phys_end;

    uint64_t k_phys_start_page = k_phys_start & ~(PAGE_SIZE - 1);
    uint64_t k_phys_end_page = (k_phys_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t pages = (k_phys_end_page - k_phys_start_page) / PAGE_SIZE;

    for (uint64_t i = 0; i < pages; i++) {
        uint64_t phys = k_phys_start_page + i * PAGE_SIZE;
        uint64_t virt = KERNEL_VIRT_BASE + (phys - k_phys_start_page);
        vmm_map_kernel(kernel_pml4, virt, phys);
    }
}

bool g_klog_screen = false;
bool* klog_to_screen = NULL;
void on_keyboard_pressed(uint8_t scancode, uint8_t final_char);

__attribute__((ms_abi, target("no-sse"), target("general-regs-only"))) void
kmain(void* params);

__attribute__((ms_abi, target("no-sse"), target("general-regs-only"))) void
u_entry(void* params) {
    boot_params_t* lp_params = (boot_params_t*)params;
    kernel_params = *lp_params;

    serial_init(COM1);
    serial_puts("u_entry: low-half bootstrap\n");

    mem_init();

    pmm_init((void*)kernel_params.memory_map_addr,
             kernel_params.memory_map_size, kernel_params.descriptor_size);

    pmm_reserve_area(0x10000000ULL, 256ULL * 1024 * 1024);

    vmm_init();
    map_kernel_high_half();

    uint64_t low_addr = (uint64_t)kmain;
    uint64_t k_phys_start_pg =
        ((uint64_t)&_kernel_phys_start) & ~(PAGE_SIZE - 1);
    uint64_t high_addr = KERNEL_VIRT_BASE + (low_addr - k_phys_start_pg);

    serial_puthex64((uint64_t)kmain);
    serial_puts(" low\n");

    serial_puthex64(high_addr);
    serial_puts(" high\n");

    asm volatile("mov %0, %%rax \n\t"
                 "jmp *%%rax    \n\t"
                 :
                 : "r"(high_addr)
                 : "rax");

    while (1) {
    }
}

// 内核主程序
__attribute__((ms_abi, target("no-sse"), target("general-regs-only"))) void
kmain(void* params) {
    (void)params;

    // 高半区调试信息
    serial_puts("kmain: now in high-half\n");
    serial_puthex64(kernel_params.memory_map_addr);
    serial_puts("\n");
    serial_puthex64(kernel_params.memory_map_size);
    serial_puts("\n");
    serial_puthex64(kernel_params.descriptor_size);
    serial_puts("\n");
    serial_puts("bpp:");
    serial_putdec64(kernel_params.framebuffer_bpp);
    serial_puts("\n");

    // 图形系统初始化
    kinfo("SYS", "Initializing graphics subsystem");
    graphics_init(&kernel_params);

    // boot splash显示
    boot_splash_init();
    boot_splash_log("MWOS Kernel Starting", 0xFFFFFFFF);
    boot_splash_set_progress(5);
    boot_splash_present();

    // gdt加载
    kinfo("GDT", "Initializing Global Descriptor Table");
    gdt_init();
    boot_splash_log("GDT initialized", 0x88FF88);
    boot_splash_set_progress(10);
    boot_splash_present();

    // idt加载
    kinfo("IDT", "Initializing Interrupt Descriptor Table");
    idt_init();
    boot_splash_log("IDT initialized", 0x88FF88);
    boot_splash_set_progress(15);
    boot_splash_present();

    // 配置 syscall 指令 (IA32_STAR, IA32_LSTAR, IA32_SF_MASK, IA32_EFER)
    {
        uint64_t star, lstar, sfmask, efer;
        uint32_t eax, edx;

        /* 1. 启用 IA32_EFER.SCE (System Call Enable) */
        asm volatile("rdmsr" : "=a"(eax), "=d"(edx) : "c"(0xC0000080));
        efer = ((uint64_t)edx << 32) | eax;
        efer |= 1;  /* bit 0 = SCE */
        eax = (uint32_t)efer;
        edx = (uint32_t)(efer >> 32);
        asm volatile("wrmsr" : : "c"(0xC0000080), "a"(eax), "d"(edx));

        /* 2. IA32_STAR[47:32] = SEL_KCODE (0x08) → SYSCALL CS, SS = 0x10
         *    IA32_STAR[63:48] = 0x13 → SYSRET CS = 0x13+16 = 0x23 (SEL_UCODE)
         *                              SYSRET SS = 0x13+8  = 0x1B (SEL_UDATA) */
        star = (0x13ULL << 48) | ((uint64_t)SEL_KCODE << 32);
        eax = (uint32_t)star;
        edx = (uint32_t)(star >> 32);
        asm volatile("wrmsr" : : "c"(0xC0000081), "a"(eax), "d"(edx));

        /* 3. IA32_LSTAR = syscall entry point */
        lstar = (uint64_t)syscall_entry;
        eax = (uint32_t)lstar;
        edx = (uint32_t)(lstar >> 32);
        asm volatile("wrmsr" : : "c"(0xC0000082), "a"(eax), "d"(edx));

        /* 4. IA32_SF_MASK = 0 (don't mask any RFLAGS bits) */
        sfmask = 0;
        eax = (uint32_t)sfmask;
        edx = (uint32_t)(sfmask >> 32);
        asm volatile("wrmsr" : : "c"(0xC0000084), "a"(eax), "d"(edx));

        kinfo("SYSCALL", "MSR syscall configured (EFER.SCE=1 STAR=0x%lx LSTAR=0x%lx)", star, lstar);
    }
    boot_splash_log("SYSCALL MSR configured", 0x88FF88);
    boot_splash_set_progress(17);
    boot_splash_present();

    // pic加载
    kinfo("PIC", "Remapping PIC: IRQs at 0x20-0x2F");
    pic_remap(32, 40);
    boot_splash_log("PIC remapped", 0x88FF88);
    boot_splash_set_progress(20);
    boot_splash_present();

    // 计时器加载
    kinfo("TIMER", "Initializing PIT at 1000 Hz");
    timer_init(1000);
    boot_splash_log("PIT initialized", 0x88FF88);
    boot_splash_set_progress(25);
    boot_splash_present();

    // pci总线扫描
    kinfo("PCI", "Scanning PCI bus");
    pci_scan_bus();
    boot_splash_log("PCI bus scanned", 0x88FF88);
    boot_splash_set_progress(35);
    boot_splash_present();

    // 磁盘控制器加载
    kinfo("DISK", "Initializing disk controllers");
    disk_init();
    boot_splash_log("Disk controllers initialized", 0x88FF88);
    boot_splash_set_progress(45);
    boot_splash_present();

    // 初始化文件系统层
    kinfo("FS", "Initializing VFS and detecting filesystem");
    fscache_init();
    vfs_init();
    ext2_register();

    uint32_t lba = vfs_detect_partition();
    if (!vfs_mount("ext2", lba)) {
        kerror("FS", "EXT2 mount failed: %s", vfs_get_error());
        boot_splash_log("EXT2 mount FAILED", 0xFF6666);
    } else {
        kinfo("FS", "EXT2 filesystem mounted successfully at LBA=%u", lba);
        boot_splash_log("EXT2 filesystem mounted", 0x88FF88);
    }
    boot_splash_set_progress(55);
    boot_splash_present();

    // 加载字体系统和默认字体
    g_klog_screen = false;
    klog_to_screen = false;
    font_manager_init();

    g_klog_font = ttf_load_from_path("/sys/fonts/MN-L.ttf");
    g_font = g_klog_font;
    if (!g_font) {
        kerror("FONT", "Failed to load window system font, text rendering will "
                       "be disabled");
        boot_splash_log("Font loading FAILED", 0xFF6666);
    } else {
        kinfo("FONT", "Window system font loaded successfully");
        boot_splash_log("System font loaded", 0x88FF88);
    }
    boot_splash_set_progress(65);
    boot_splash_present();

    // 初始化ps/2键盘
    kinfo("INPUT", "Initializing PS/2 keyboard");
    keyboard_init();
    boot_splash_log("Keyboard initialized", 0x88FF88);
    boot_splash_set_progress(70);
    boot_splash_present();

    // ps/2鼠标
    kinfo("INPUT", "Initializing PS/2 mouse");
    mouse_init();
    boot_splash_log("Mouse initialized", 0x88FF88);
    boot_splash_set_progress(75);
    boot_splash_present();

    // 加载pic
    outb(0x21, 0xF8);
    outb(0xA1, 0xEF);
    asm volatile("sti");
    kinfo("PIC", "PIC interrupt masking configured");
    boot_splash_set_progress(80);
    boot_splash_present();

    // 加载终端
    kinfo("SHELL", "Initializing command shell");
    boot_splash_set_progress(85);
    boot_splash_present();

    // 加载wm
    screen_width = kernel_params.framebuffer_width;
    screen_height = kernel_params.framebuffer_height;
    kinfo("WM", "Initializing window manager: %ux%u", screen_width,
          screen_height);
    g_klog_screen = false;
    wm_init(screen_width, screen_height);
    ui_init();
    wm_desktop_init();
    boot_splash_log("Window manager initialized", 0x88FF88);
    boot_splash_set_progress(100);
    boot_splash_present();
    screen_width = kernel_params.framebuffer_width;
    screen_height = kernel_params.framebuffer_height;

    // 初始化终端模块
    kinfo("TERM", "Initializing terminal module");
    terminal_init();

    // 初始化 shell（无默认终端窗口，点击桌面按钮后创建）
    kinfo("TERM", "Initializing shell");
    shell_init();
    shell_set_output(terminal_output);
    g_elf_on_exit = shell_print_prompt;

    wm_redraw();
    graphics_present();

    // 初始化 TSS（供后续 elfexec 使用）
    tss_init();
    uint64_t kernel_rsp;
    asm volatile("mov %%rsp, %0" : "=r"(kernel_rsp));
    tss_set_stack(kernel_rsp);

    kinfo("SYS", "MWOS initialization complete. Entering main loop.");
    for (;;) {
        /* 先清除上一轮的鼠标光标，再重绘窗口内容 */
        if (g_old_mouse_x != -1)
            mouse_restore_bg(g_old_mouse_x, g_old_mouse_y);
        wm_redraw_dirty();
        /* 保存干净背景并绘制新光标 */
        mouse_save_bg(mouse_x, mouse_y);
        mouse_draw(mouse_x, mouse_y);
        g_old_mouse_x = mouse_x;
        g_old_mouse_y = mouse_y;
        asm volatile("hlt");
    }
}

void on_keyboard_pressed(uint8_t scancode, uint8_t final_char) {
    if (final_char == 0)
        return;

    if (final_char == '\r')
        final_char = '\n';
    else if (final_char == 0x7F)
        final_char = '\b';

    shell_process_char(final_char);
}

static int preview_last_valid = 0;
static int preview_last_x = 0;
static int preview_last_y = 0;

void on_mouse_update(int32_t x_rel, int32_t y_rel, uint8_t left_button,
                     uint8_t middle_button, uint8_t right_button) {
    (void)middle_button;
    (void)right_button;

    mouse_x += x_rel;
    mouse_y -= y_rel;

    if (mouse_x < 0)
        mouse_x = 0;
    if (mouse_y < 0)
        mouse_y = 0;

    if (mouse_x >= (int32_t)kernel_params.framebuffer_width)
        mouse_x = kernel_params.framebuffer_width - 1;

    if (mouse_y >= (int32_t)kernel_params.framebuffer_height)
        mouse_y = kernel_params.framebuffer_height - 1;

    if (g_old_mouse_x != -1)
        mouse_restore_bg(g_old_mouse_x, g_old_mouse_y);

    bool moved = wm_handle_mouse(mouse_x, mouse_y, left_button);

    uint32_t pw = 0, ph = 0;
    if (g_dragging_window) {
        pw = g_dragging_window->width;
        ph = g_dragging_window->height + g_dragging_window->title_height;
    }

    uint32_t xor_color = 0x00FFFFFF;

    if (preview_active && moved) {
        if (preview_last_valid) {
            xor_rect_fb(preview_last_x, preview_last_y, pw, ph, xor_color);
        }

        xor_rect_fb(preview_x, preview_y, pw, ph, xor_color);

        preview_last_x = preview_x;
        preview_last_y = preview_y;
        preview_last_valid = 1;
    } else if (!preview_active && moved) {
        if (preview_last_valid) {
            xor_rect_fb(preview_last_x, preview_last_y, pw, ph, xor_color);
            preview_last_valid = 0;
        }
        wm_redraw_dirty();
    }

    mouse_save_bg(mouse_x, mouse_y);
    mouse_draw(mouse_x, mouse_y);

    g_old_mouse_x = mouse_x;
    g_old_mouse_y = mouse_y;
}
