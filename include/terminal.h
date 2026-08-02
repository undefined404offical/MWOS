#ifndef TERMINAL_H
#define TERMINAL_H

#include "wm.h"
#include <stdbool.h>
#include <stdint.h>

#define TERM_MAX_LINES 512
#define TERM_MAX_LINE_LEN 256
#define TERM_FONT_SIZE 16
#define TERM_LINE_HEIGHT 20
#define TERM_MARGIN_X 10
#define TERM_MARGIN_Y 35

typedef struct {
    char lines[TERM_MAX_LINES][TERM_MAX_LINE_LEN];
    uint32_t colors[TERM_MAX_LINES];
    int line_count;
    int scroll_offset;
    int cursor_line;
    int cursor_col;

    char line_buffer[TERM_MAX_LINE_LEN];
    int line_buffer_len;
    uint32_t line_buffer_color;

    wm_window_t* window;
    bool initialized;
} terminal_t;

extern terminal_t* g_active_terminal;

void terminal_init(void);
terminal_t* terminal_create_window(int x, int y, int width, int height);
void terminal_set_active(terminal_t* term);
void terminal_output(const char* str);
void terminal_clear(void);
void terminal_refresh(void);
void terminal_scroll(int delta);
void terminal_set_color(uint32_t color);

terminal_t* terminal_get(void);
wm_window_t* terminal_get_window(void);

#endif
