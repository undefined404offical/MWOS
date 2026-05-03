/*
                   _ooOoo_
                  o8888888o
                  88" . "88
                  (| -_- |)
                  O\  =  /O
               ____/`---'\____
             .'  \\|     |//  `.
            /  \\|||  :  |||//  \
           /  _||||| -:- |||||-  \
           |   | \\\  -  /// |   |
           | \_|  ''\---/''  |   |
           \  .-\__  `-`  ___/-. /
         ___`. .'  /--.--\  `. . __
      ."" '<  `.___\_<|>_/___.'  >'"".
     | | :  `- \`.;`\ _ /`;.`/ - ` : | |
     \  \ `-.   \_ __\ /__ _/   .-` /  /
======`-.____`-.___\_____/___.-`____.-'======
                   `=---='
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
            佛祖保佑       永无BUG

              ,----------------,              ,---------,
         ,-----------------------,          ,"        ,"|
       ,"                      ,"|        ,"        ,"  |
      +-----------------------+  |      ,"        ,"    |
      |  .-----------------.  |  |     +---------+      |
      |  |                 |  |  |     | -==----'|      |
      |  |  Never gonna    |  |  |     |         |      |
      |  |  Give you bug   |  |  |/----|`---=    |      |
      |  |  C:\>_          |  |  |   ,/|==== ooo |      ;
      |  |                 |  |  |  // |(((( [33]|    ,"
      |  `-----------------'  |," .;'| |((((     |  ,"
      +-----------------------+  ;;  | |         |,"
         /_)______________(_/  //'   | +---------+
    ___________________________/___  `,
   /  oooooooooooooooo  .o.  oooo /,   \,"-----------
  / ==ooooooooooooooo==.o.  ooo= //   ,`\--{)B     ,"
 /_==__==========__==_ooo__ooo=_/'   /___________,"
*/

#include "boot_splash.h"
#include "drivers/disk.h"
#include "drivers/fs/fat32.h"
#include "drivers/fs/fscache.h"
#include "drivers/pci.h"
#include "font_manager.h"
#include "graphics.h"
#include "kernel.h"
#include "klog.h"
#include "memory.h"
#include "shell.h"
#include "stdint.h"
#include "string.h"
#include "thread.h"
#include "ttf.h"
#include "ttf_cache.h"
#include "ui/microui.h"
#include "vmm.h"
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

// =========================
// 终端窗口
// =========================

#define TERM_MAX_LINES 256
#define TERM_MAX_LINE_LEN 256
#define TERM_FONT_SIZE 16
#define TERM_LINE_HEIGHT 20
#define TERM_MARGIN_X 10
#define TERM_MARGIN_Y 35

static char g_term_lines[TERM_MAX_LINES][TERM_MAX_LINE_LEN];
static uint32_t g_term_colors[TERM_MAX_LINES]; // 每行的颜色
static int g_term_line_count = 0;
static int g_term_scroll = 0;
static wm_window_t* g_term_win = NULL;
bool g_terminal_initialized = false;
void (*g_klog_term_output)(const char* str) = NULL;

static void term_append_line(const char* str, uint32_t color) {
    if (g_term_line_count >= TERM_MAX_LINES) {
        for (int i = 0; i < TERM_MAX_LINES - 1; i++) {
            memcpy(g_term_lines[i], g_term_lines[i + 1], TERM_MAX_LINE_LEN);
            g_term_colors[i] = g_term_colors[i + 1]; // 同时移动颜色
        }
        g_term_line_count = TERM_MAX_LINES - 1;
    }

    int len = 0;
    while (str[len] && len < TERM_MAX_LINE_LEN - 1) {
        if (str[len] == '\n' || str[len] == '\r')
            break;
        g_term_lines[g_term_line_count][len] = str[len];
        len++;
    }
    g_term_lines[g_term_line_count][len] = '\0';
    g_term_colors[g_term_line_count] = color;
    g_term_line_count++;
    g_term_scroll = g_term_line_count;
}

static void term_append_char(char c, uint32_t color) {
    if (g_term_line_count == 0) {
        g_term_lines[0][0] = '\0';
        g_term_colors[0] = color;
        g_term_line_count = 1;
    }

    int last = g_term_line_count - 1;
    int len = 0;
    while (g_term_lines[last][len])
        len++;

    if (len < TERM_MAX_LINE_LEN - 1) {
        g_term_lines[last][len] = c;
        g_term_lines[last][len + 1] = '\0';
    }
    g_term_colors[last] = color;
    g_term_scroll = g_term_line_count;
}

static void term_backspace(void) {
    if (g_term_line_count == 0)
        return;

    int last = g_term_line_count - 1;
    int len = 0;
    while (g_term_lines[last][len])
        len++;

    if (len > 0) {
        g_term_lines[last][len - 1] = '\0';
    }
    g_term_scroll = g_term_line_count;
}

// 解析ANSI颜色代码并转换为颜色值
static uint32_t parse_ansi_color(const char* code) {
    if (strncmp(code, "[30m", 4) == 0)
        return 0x000000; // 黑色
    if (strncmp(code, "[31m", 4) == 0)
        return 0xFF0000; // 红色
    if (strncmp(code, "[32m", 4) == 0)
        return 0x00FF00; // 绿色
    if (strncmp(code, "[33m", 4) == 0)
        return 0xFFFF00; // 黄色
    if (strncmp(code, "[34m", 4) == 0)
        return 0x0000FF; // 蓝色
    if (strncmp(code, "[35m", 4) == 0)
        return 0xFF00FF; // 紫色
    if (strncmp(code, "[36m", 4) == 0)
        return 0x00FFFF; // 青色
    if (strncmp(code, "[37m", 4) == 0)
        return 0xFFFFFF; // 白色
    if (strncmp(code, "[0m", 3) == 0)
        return 0xFFFFFF; // 重置为白色
    return 0xFFFFFF;
}

static void term_window_draw(wm_window_t* win);

static void term_output_callback(const char* str) {
    if (!str)
        return;

    const char* p = str;
    char line_buf[TERM_MAX_LINE_LEN];
    int line_len = 0;
    uint32_t current_color = 0xFFFFFF;
    bool has_newline = false;

    while (*p) {
        if (*p == '\033' && *(p + 1) == '[') {
            current_color = parse_ansi_color(p + 2);
            while (*p && *p != 'm')
                p++;
            if (*p == 'm')
                p++;
            continue;
        }

        if (*p == '\n' || *p == '\r') {
            has_newline = true;
            if (line_len > 0) {
                line_buf[line_len] = '\0';
                term_append_line(line_buf, current_color);
                line_len = 0;
            }
            p++;
        } else if (*p == '\b') {
            if (line_len > 0) {
                line_len--;
            } else {
                term_backspace();
            }
            p++;
        } else {
            if (line_len < TERM_MAX_LINE_LEN - 1)
                line_buf[line_len++] = *p;
            p++;
        }
    }

    if (line_len > 0) {
        line_buf[line_len] = '\0';
        if (has_newline) {
            term_append_line(line_buf, current_color);
        } else {
            for (int i = 0; line_buf[i]; i++) {
                term_append_char(line_buf[i], current_color);
            }
        }
    }

    if (g_term_win) {
        term_window_draw(g_term_win);
        wm_invalidate_window(g_term_win);
        wm_redraw_dirty();
        graphics_present();
    }
}

static void term_window_draw(wm_window_t* win) {
    if (!win->buffer)
        return;

    // 绘制窗口背景
    for (int i = 0; i < win->buf_width * win->buf_height; i++) {
        win->buffer[i] = 0xFF202020; // 浅灰色背景
    }

    // 如果没有字体，使用简单的矩形显示文字位置
    if (!g_font) {
        int content_h = win->height;
        int max_visible = content_h / TERM_LINE_HEIGHT;
        int start = g_term_scroll - max_visible;
        if (start < 0)
            start = 0;

        int y = TERM_MARGIN_Y;
        for (int i = start; i < g_term_line_count && y < content_h; i++) {
            // 绘制文字位置的矩形框
            for (int py = y; py < y + TERM_LINE_HEIGHT && py < win->buf_height;
                 py++) {
                for (int px = TERM_MARGIN_X;
                     px < TERM_MARGIN_X + 200 && px < win->buf_width; px++) {
                    win->buffer[py * win->buf_width + px] =
                        0xFFFF0000; // 红色矩形表示文字位置
                }
            }
            y += TERM_LINE_HEIGHT;
        }
        return;
    }

    // 使用字体绘制文字
    int content_h = win->height;
    int max_visible = content_h / TERM_LINE_HEIGHT;

    int start = g_term_scroll - max_visible;
    if (start < 0)
        start = 0;

    int y = TERM_MARGIN_Y;

    for (int i = start; i < g_term_line_count && y < content_h; i++) {
        ttf_draw_text_utf8_buf(g_font, TERM_MARGIN_X, y, TERM_FONT_SIZE,
                               g_term_colors[i], // 使用每行对应的颜色
                               win->buffer, win->buf_width, win->buf_height,
                               g_term_lines[i]);
        y += TERM_LINE_HEIGHT;
    }
}

static void term_window_mouse(wm_window_t* win, int lx, int ly, int button) {
    (void)win;
    (void)lx;
    (void)ly;
    (void)button;
}

// =========================
// 高半区内核跳板所需声明
// =========================

#include <stdint.h>

#define KERNEL_VIRT_BASE 0xFFFFFFFF80000000ULL

// 由链接脚本提供（必须在 linker.ld 里定义）
extern uint8_t _kernel_phys_start;
extern uint8_t _kernel_phys_end;

// vmm_init() 里必须把 kernel_pml4 赋值到这个全局变量
extern pt_entry_t* kernel_pml4;

// 高半区映射函数（必须放在 kmain.c 里）
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

// 这个用来跳到高半区
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

// 这里是高半区内核
__attribute__((ms_abi, target("no-sse"), target("general-regs-only"))) void
kmain(void* params) {
    // 输出高半区内存映射信息
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

    kinfo("SYS", "Initializing graphics subsystem");
    graphics_init(&kernel_params);

    boot_splash_init();
    boot_splash_log("MWOS Kernel Starting", 0xFFFFFFFF);
    boot_splash_set_progress(5);
    boot_splash_present();

    kinfo("GDT", "Initializing Global Descriptor Table");
    gdt_init();
    boot_splash_log("GDT initialized", 0x88FF88);
    boot_splash_set_progress(10);
    boot_splash_present();

    kinfo("IDT", "Initializing Interrupt Descriptor Table");
    idt_init();
    boot_splash_log("IDT initialized", 0x88FF88);
    boot_splash_set_progress(15);
    boot_splash_present();

    kinfo("PIC", "Remapping PIC: IRQs at 0x20-0x2F");
    pic_remap(32, 40);
    boot_splash_log("PIC remapped", 0x88FF88);
    boot_splash_set_progress(20);
    boot_splash_present();

    kinfo("TIMER", "Initializing PIT at 1000 Hz");
    timer_init(1000);
    boot_splash_log("PIT initialized", 0x88FF88);
    boot_splash_set_progress(25);
    boot_splash_present();

    kinfo("PCI", "Scanning PCI bus");
    pci_scan_bus();
    boot_splash_log("PCI bus scanned", 0x88FF88);
    boot_splash_set_progress(35);
    boot_splash_present();

    kinfo("DISK", "Initializing disk controllers");
    disk_init();
    boot_splash_log("Disk controllers initialized", 0x88FF88);
    boot_splash_set_progress(45);
    boot_splash_present();

    kinfo("FS", "Detecting and mounting FAT32 filesystem");
    uint32_t lba = detect_fat32_partition();

    fscache_init();

    if (!fat32_mount(lba)) {
        kerror("FS", "FAT32 mount failed: %s", fat32_get_error());
        boot_splash_log("FAT32 mount FAILED", 0xFF6666);
    } else {
        kinfo("FS", "FAT32 filesystem mounted successfully at LBA=%u", lba);
        boot_splash_log("FAT32 filesystem mounted", 0x88FF88);
    }
    boot_splash_set_progress(55);
    boot_splash_present();

    kinfo("FONT", "Loading system font");
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

    ttf_cache_init();
    if (g_font) {
        ttf_cache_preload_ascii(g_font, 16, 0xFFFFFF);
    }

    kinfo("INPUT", "Initializing PS/2 keyboard");
    keyboard_init();
    boot_splash_log("Keyboard initialized", 0x88FF88);
    boot_splash_set_progress(70);
    boot_splash_present();

    kinfo("INPUT", "Initializing PS/2 mouse");
    mouse_init();
    boot_splash_log("Mouse initialized", 0x88FF88);
    boot_splash_set_progress(75);
    boot_splash_present();

    outb(0x21, 0xF8);
    outb(0xA1, 0xEF);
    asm volatile("sti");

    kinfo("PIC", "PIC interrupt masking configured");
    boot_splash_set_progress(80);
    boot_splash_present();

    kinfo("SHELL", "Initializing command shell");
    shell_set_term_output(term_output_callback);
    shell_init();
    boot_splash_log("Shell initialized", 0x88FF88);
    boot_splash_set_progress(85);
    boot_splash_present();

    screen_width = kernel_params.framebuffer_width;
    screen_height = kernel_params.framebuffer_height;

    kinfo("WM", "Initializing window manager: %ux%u", screen_width,
          screen_height);
    g_klog_screen = false;
    wm_init(screen_width, screen_height);
    ui_init();
    boot_splash_log("Window manager initialized", 0x88FF88);
    boot_splash_set_progress(100);
    boot_splash_present();

    screen_width = kernel_params.framebuffer_width;
    screen_height = kernel_params.framebuffer_height;

    kinfo("WM", "Creating Terminal window");
    g_term_win = wm_create_window(50, 50, 1200, 700, "Terminal",
                                  term_window_draw, term_window_mouse);

    for (int i = 0; i < TERM_MAX_LINES; i++) {
        g_term_colors[i] = 0xFFFFFF;
    }

    term_append_line("MWOS Terminal - Welcome!", 0xFFFFFF);
    term_append_line("Type 'help' for available commands", 0x00FF00);
    term_append_line(g_font ? "Font status: Loaded"
                            : "Font status: Not available",
                     0xFFFF00);
    term_append_line("", 0xFFFFFF);

    serial_puts("TERM: Font status: ");
    serial_puts(g_font ? "Loaded" : "Not available");
    serial_puts("\n");
    serial_puts("TERM: Window created at ");
    serial_putdec32(g_term_win ? g_term_win->x : 0);
    serial_puts(",");
    serial_putdec32(g_term_win ? g_term_win->y : 0);
    serial_puts(" size ");
    serial_putdec32(g_term_win ? g_term_win->width : 0);
    serial_puts("x");
    serial_putdec32(g_term_win ? g_term_win->height : 0);
    serial_puts("\n");

    if (g_term_win) {
        wm_invalidate_window(g_term_win);
        wm_redraw();
        graphics_present();

        g_terminal_initialized = true;
        g_klog_term_output = term_output_callback;
    }

    kinfo("THREAD", "Creating idle thread");
    thread_create(idle_thread, NULL, 0);

    kinfo("SCHED", "Starting scheduler");
    scheduler_start();

    kinfo("SYS", "MWOS initialization complete. Entering main loop.");

    for (;;) {
        wm_redraw_dirty();
        graphics_present();
        mouse_save_bg(mouse_x, mouse_y);
        mouse_draw(mouse_x, mouse_y);
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
    // 1. 更新鼠标坐标
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

    // 2. 恢复旧鼠标背景
    if (g_old_mouse_x != -1)
        mouse_restore_bg(g_old_mouse_x, g_old_mouse_y);

    // 3. 处理窗口拖动逻辑（只算 preview_x / preview_y / g_dragging_window）
    bool moved = wm_handle_mouse(mouse_x, mouse_y, left_button);

    uint32_t pw = 0, ph = 0;
    if (g_dragging_window) {
        pw = g_dragging_window->width;
        ph = g_dragging_window->height + g_dragging_window->title_height;
    }

    uint32_t xor_color = 0x00FFFFFF; // 低 24 位参与 XOR，alpha 随便

    // 4. 拖动中：用 XOR 框，先擦旧，再画新
    if (preview_active && moved) {

        // 4.1 擦掉上一帧的框（同样的 XOR 再画一次）
        if (preview_last_valid) {
            xor_rect_fb(preview_last_x, preview_last_y, pw, ph, xor_color);
        }

        // 4.2 画本帧的新框
        xor_rect_fb(preview_x, preview_y, pw, ph, xor_color);

        // 4.3 记录本帧位置
        preview_last_x = preview_x;
        preview_last_y = preview_y;
        preview_last_valid = 1;
    }

    // 5. 松手：擦掉框 + 真正移动窗口
    else if (!preview_active && moved) {

        // 5.1 如果之前有框，先用 XOR 擦掉
        if (preview_last_valid) {
            xor_rect_fb(preview_last_x, preview_last_y, pw, ph, xor_color);
            preview_last_valid = 0;
        }

        // 5.2 真正重绘窗口
        wm_redraw();
    }

    // 6. 最后画鼠标（永远在最上层）
    mouse_save_bg(mouse_x, mouse_y);
    mouse_draw(mouse_x, mouse_y);

    g_old_mouse_x = mouse_x;
    g_old_mouse_y = mouse_y;
}
