#ifndef PS2_MOUSE_H
#define PS2_MOUSE_H

#include <stdint.h>
#include "idt.h"

// PS/2 端口
#define MOUSE_DATA_PORT      0x60
#define MOUSE_STATUS_REG     0x64
#define MOUSE_COMMAND_REG    0x64

// 鼠标状态位定义 (Byte 0)
#define MOUSE_LEFT_BUTTON   0x01
#define MOUSE_RIGHT_BUTTON  0x02
#define MOUSE_MIDDLE_BUTTON 0x04
#define MOUSE_X_SIGN        0x10  // 1 = 负, 0 = 正
#define MOUSE_Y_SIGN        0x20  // 1 = 负, 0 = 正
#define MOUSE_X_OVERFLOW    0x40
#define MOUSE_Y_OVERFLOW    0x80

typedef struct {
    int32_t x;
    int32_t y;
    uint8_t left_button;
    uint8_t right_button;
    uint8_t middle_button;
} mouse_state_t;

#define CURSOR_W 16
#define CURSOR_H 24

extern const char mouse_cursor[CURSOR_H][CURSOR_W];

void mouse_init(void);

mouse_state_t* get_mouse_state(void);

// 旧式鼠标绘制接口（WM 不再使用，但保留以防你其他地方引用）
void draw_mouse_cursor(int x, int y);
void save_mouse_background(int x, int y);
void restore_mouse_background();

#endif // PS2_MOUSE_H
