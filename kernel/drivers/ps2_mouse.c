#include "drivers/ps2_mouse.h"
#include "io.h"
#include "serial.h"
#include "cstd.h"
#include "graphics.h"

#define CURSOR_W 16
#define CURSOR_H 24

static uint32_t mouse_back_buffer[CURSOR_W * CURSOR_H];
static int last_back_x = 0;
static int last_back_y = 0;
static int last_back_w = 0;
static int last_back_h = 0;

static uint8_t  mouse_cycle = 0;
static uint8_t  mouse_packet[3];
static mouse_state_t current_mouse = {0, 0, 0, 0, 0};

const char mouse_cursor[CURSOR_H][CURSOR_W] = {
    {'X',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' '},
    {'X','X',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' '},
    {'X','.','X',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' '},
    {'X','.','.','X',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' '},
    {'X','.','.','.','X',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' '},
    {'X','.','.','.','.','X',' ',' ',' ',' ',' ',' ',' ',' ',' ',' '},
    {'X','.','.','.','.','.','X',' ',' ',' ',' ',' ',' ',' ',' ',' '},
    {'X','.','.','.','.','.','.','X',' ',' ',' ',' ',' ',' ',' ',' '},
    {'X','.','.','.','.','.','.','.','X',' ',' ',' ',' ',' ',' ',' '},
    {'X','.','.','.','.','.','.','.','.','X',' ',' ',' ',' ',' ',' '},
    {'X','.','.','.','.','.','.','.','.','.','X',' ',' ',' ',' ',' '},
    {'X','.','.','.','.','.','.','.','.','.','.','X',' ',' ',' ',' '},
    {'X','.','.','.','.','.','.','.','.','.','.','.','X',' ',' ',' '},
    {'X','.','.','.','.','.','.','.','.','.','.','.','.','X',' ',' '},
    {'X','.','.','.','.','.','.','X','X','X','X','X','X',' ',' ',' '},
    {'X','.','.','X','.','.','.','.','X',' ',' ',' ',' ',' ',' ',' '},
    {'X','.','X',' ','X','.','.','.','X',' ',' ',' ',' ',' ',' ',' '},
    {'X','X',' ',' ','X','.','.','.','X',' ',' ',' ',' ',' ',' ',' '},
    {'X',' ',' ',' ',' ','X','.','.','.','X',' ',' ',' ',' ',' ',' '},
    {' ',' ',' ',' ',' ','X','.','.','.','X',' ',' ',' ',' ',' ',' '},
    {' ',' ',' ',' ',' ',' ','X','.','.','.','X',' ',' ',' ',' ',' '},
    {' ',' ',' ',' ',' ',' ','X','.','.','X','X',' ',' ',' ',' ',' '},
    {' ',' ',' ',' ',' ',' ',' ','X','X',' ',' ',' ',' ',' ',' ',' '},
    {' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' '}
};

// 等待 PS/2 控制器就绪
static void mouse_wait(uint8_t type) {
    uint32_t timeout = 100000;
    if (type == 0) { // 等待读取
        while (timeout-- && !(inb(MOUSE_STATUS_REG) & 1));
    } else { // 等待写入
        while (timeout-- && (inb(MOUSE_STATUS_REG) & 2));
    }
}

static void mouse_write(uint8_t data) {
    mouse_wait(1);
    outb(MOUSE_COMMAND_REG, 0xD4); // 告知下个字节发给鼠标
    mouse_wait(1);
    outb(MOUSE_DATA_PORT, data);
}

static uint8_t mouse_read() {
    mouse_wait(0);
    return inb(MOUSE_DATA_PORT);
}

void mouse_handler(interrupt_frame_t *frame) {
    uint8_t status = inb(MOUSE_STATUS_REG);

    // 检查是否有数据且数据来自鼠标 (Bit 5)
    if (!(status & 0x01) || !(status & 0x20)) return;

    mouse_packet[mouse_cycle++] = inb(MOUSE_DATA_PORT);

    if (mouse_cycle == 3) {
        mouse_cycle = 0;

        // 规范化按键为 0/1
        uint8_t left_mask   = (mouse_packet[0] & MOUSE_LEFT_BUTTON) ? 1 : 0;
        uint8_t right_mask  = (mouse_packet[0] & MOUSE_RIGHT_BUTTON) ? 1 : 0;
        uint8_t middle_mask = (mouse_packet[0] & MOUSE_MIDDLE_BUTTON) ? 1 : 0;

        // 解析 X/Y 位移（用 int8_t 做符号扩展更直观）
        int8_t dx8 = (int8_t)mouse_packet[1];
        int8_t dy8 = (int8_t)mouse_packet[2];
        int32_t x_rel = (int32_t)dx8;
        int32_t y_rel = (int32_t)dy8;

        // PS/2 的 Y 是向上为正，若你的上层期望 y_rel 向上为正，保留；否则取反
        // 你原来 on_mouse_update 用的是 mouse_y -= y_rel，所以保持一致即可

        // 检查按键边沿（可选：用于调试）
        static uint8_t prev_left = 0;
        static uint8_t prev_right = 0;
        static uint8_t prev_middle = 0;

        if (left_mask != prev_left) {
            if (left_mask)
                serial_puts("MOUSE: left down\n");
            else
                serial_puts("MOUSE: left up\n");
            prev_left = left_mask;
        }
        if (right_mask != prev_right) {
            if (right_mask)
                serial_puts("MOUSE: right down\n");
            else
                serial_puts("MOUSE: right up\n");
            prev_right = right_mask;
        }
        if (middle_mask != prev_middle) {
            if (middle_mask)
                serial_puts("MOUSE: middle down\n");
            else
                serial_puts("MOUSE: middle up\n");
            prev_middle = middle_mask;
        }

        // 更新全局状态（保持兼容你的其它接口）
        current_mouse.left_button   = left_mask;
        current_mouse.right_button  = right_mask;
        current_mouse.middle_button = middle_mask;

        on_mouse_update(x_rel, y_rel, (uint8_t)left_mask, (uint8_t)middle_mask, (uint8_t)right_mask);
    }
}

void mouse_init(void) {
    uint8_t status;

    mouse_wait(1);
    outb(MOUSE_COMMAND_REG, 0xA8);

    mouse_wait(1);
    outb(MOUSE_COMMAND_REG, 0x20); // 读配置指令
    mouse_wait(0);
    status = (inb(MOUSE_DATA_PORT) | 2); // 启用第 2 中断源 (IRQ12)
    mouse_wait(1);
    outb(MOUSE_COMMAND_REG, 0x60); // 写配置指令
    mouse_wait(1);
    outb(MOUSE_DATA_PORT, status);

    mouse_write(0xF6); // Set Default
    mouse_read();      // ACK
    mouse_write(0xF3); // 设置采样率命令
    mouse_read();      // 等待 ACK (0xFA)
    mouse_write(100);  // 设置为 100 Hz
    mouse_read();      // 等待 ACK (0xFA)
    mouse_write(0xF4); // Enable
    mouse_read();      // ACK

    // 0xF9 = 11111001 (开启 IRQ1 键盘 和 IRQ2 级联口)
    outb(0x21, 0xF9); 
    // 0xEF = 11101111 (开启 IRQ12 鼠标)
    outb(0xA1, 0xEF);

    register_interrupt_handler(44, mouse_handler);
    
    serial_puts("PS/2 Mouse Initialized.\n");
}

mouse_state_t* get_mouse_state(void) {
    return &current_mouse;
}

void draw_mouse_cursor(int x, int y) {
    for (int row = 0; row < CURSOR_H; row++) {
        for (int col = 0; col < CURSOR_W; col++) {
            char pixel = mouse_cursor[row][col];
            
            if (pixel == 'X') {
                put_pixel(x + col, y + row, COLOR_BLACK);
            } else if (pixel == '.') {
                put_pixel(x + col, y + row, COLOR_WHITE);
            }
        }
    }
}

void save_mouse_background(int x, int y) {
    last_back_x = x;
    last_back_y = y;
    
    int real_w = CURSOR_W;
    int real_h = CURSOR_H;
    
    if (x + CURSOR_W > g_framebuffer->framebuffer_width) {
        real_w = g_framebuffer->framebuffer_width - x;
    }
    if (y + CURSOR_H > g_framebuffer->framebuffer_height) {
        real_h = g_framebuffer->framebuffer_height - y;
    }
    
    last_back_w = real_w;
    last_back_h = real_h;

    uint32_t* fb = (uint32_t*)g_framebuffer->framebuffer_addr;
    uint32_t pitch_pixels = g_framebuffer->framebuffer_pitch >> 2;

    for (int i = 0; i < real_h; i++) {
        for (int j = 0; j < real_w; j++) {
            mouse_back_buffer[i * CURSOR_W + j] = fb[(y + i) * pitch_pixels + (x + j)];
        }
    }
}

void restore_mouse_background() {
    if (last_back_w <= 0 || last_back_h <= 0) return;

    uint32_t* fb = (uint32_t*)g_framebuffer->framebuffer_addr;
    uint32_t pitch_pixels = g_framebuffer->framebuffer_pitch >> 2;

    for (int i = 0; i < last_back_h; i++) {
        for (int j = 0; j < last_back_w; j++) {
            fb[(last_back_y + i) * pitch_pixels + (last_back_x + j)] = mouse_back_buffer[i * CURSOR_W + j];
        }
    }
}
