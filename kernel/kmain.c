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
#include "terminal.h"
#include "thread.h"
#include "ttf.h"
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

__attribute__((ms_abi, target("no-sse"), target("general-regs-only"))) void
kmain(void* params) {
    (void)params;

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

    kinfo("TERM", "Initializing terminal module");
    terminal_init();

    kinfo("TERM", "Creating Terminal window");
    terminal_create_window(50, 50, 1200, 700);

    serial_puts("TERM: Font status: ");
    serial_puts(g_font ? "Loaded" : "Not available");
    serial_puts("\n");

    wm_window_t* term_win = terminal_get_window();
    if (term_win) {
        serial_puts("TERM: Window created at ");
        serial_putdec32(term_win->x);
        serial_puts(",");
        serial_putdec32(term_win->y);
        serial_puts(" size ");
        serial_putdec32(term_win->width);
        serial_puts("x");
        serial_putdec32(term_win->height);
        serial_puts("\n");

        shell_init();
        shell_set_output(terminal_output);
        shell_print_prompt();

        wm_redraw();
        graphics_present();
    } else {
        kerror("TERM", "Failed to create terminal window");
    }

    kinfo("THREAD", "Creating idle thread");
    thread_create(idle_thread, NULL, 0);

    kinfo("SCHED", "Starting scheduler");
    scheduler_start();

    kinfo("SYS", "MWOS initialization complete. Entering main loop.");

    for (;;) {
        wm_redraw_dirty();
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
