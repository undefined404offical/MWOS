#include "terminal.h"
#include "graphics.h"
#include "klog.h"
#include "serial.h"
#include "string.h"
#include "ttf.h"
#include "wm.h"
#include "memory.h"

extern TTF_Font* g_font;

void (*g_klog_term_output)(const char* str) = NULL;
bool g_terminal_initialized = false;

terminal_t* g_active_terminal = NULL;

static uint32_t parse_ansi_color(const char** p) {
    if (!p || !*p || **p != '\033')
        return 0xFFFFFF;

    (*p)++;
    if (**p == '[')
        (*p)++;

    uint32_t color = 0xFFFFFF;

    while (**p && **p != 'm') {
        if (**p >= '0' && **p <= '9') {
            int code = 0;
            while (**p >= '0' && **p <= '9') {
                code = code * 10 + (**p - '0');
                (*p)++;
            }

            switch (code) {
            case 0:
                color = 0xFFFFFF;
                break;
            case 1:
                break;
            case 30:
                color = 0x000000;
                break;
            case 31:
                color = 0xFF0000;
                break;
            case 32:
                color = 0x00FF00;
                break;
            case 33:
                color = 0xFFFF00;
                break;
            case 34:
                color = 0x0000FF;
                break;
            case 35:
                color = 0xFF00FF;
                break;
            case 36:
                color = 0x00FFFF;
                break;
            case 37:
                color = 0xFFFFFF;
                break;
            default:
                break;
            }

            if (**p == ';')
                (*p)++;
        } else {
            (*p)++;
        }
    }

    if (**p == 'm')
        (*p)++;

    return color;
}

static void term_append_line(terminal_t* t, const char* str, uint32_t color) {
    if (!t) return;

    if (t->line_count >= TERM_MAX_LINES) {
        for (int i = 0; i < TERM_MAX_LINES - 1; i++) {
            memcpy(t->lines[i], t->lines[i + 1], TERM_MAX_LINE_LEN);
            t->colors[i] = t->colors[i + 1];
        }
        t->line_count = TERM_MAX_LINES - 1;
    }

    if (str == NULL || *str == '\0') {
        t->lines[t->line_count][0] = '\0';
    } else {
        int len = 0;
        while (str[len] && len < TERM_MAX_LINE_LEN - 1) {
            if (str[len] == '\n' || str[len] == '\r')
                break;
            t->lines[t->line_count][len] = str[len];
            len++;
        }
        t->lines[t->line_count][len] = '\0';
    }

    t->colors[t->line_count] = color;
    t->line_count++;
    t->scroll_offset = t->line_count;
}

static void term_append_char(terminal_t* t, char c, uint32_t color) {
    if (!t) return;

    if (t->line_count == 0) {
        t->lines[0][0] = '\0';
        t->colors[0] = color;
        t->line_count = 1;
    }

    int last = t->line_count - 1;
    int len = 0;
    while (len < TERM_MAX_LINE_LEN - 1 && t->lines[last][len])
        len++;

    if (len < TERM_MAX_LINE_LEN - 1) {
        t->lines[last][len] = c;
        t->lines[last][len + 1] = '\0';
    }
    t->colors[last] = color;
    t->scroll_offset = t->line_count;
}

static void term_backspace(terminal_t* t) {
    if (!t) return;

    if (t->line_count == 0)
        return;

    int last = t->line_count - 1;
    int len = 0;
    while (t->lines[last][len])
        len++;

    if (len > 0) {
        t->lines[last][len - 1] = '\0';
    }
    t->scroll_offset = t->line_count;
}

static void term_draw(wm_window_t* win) {
    if (!win || !win->buffer)
        return;

    terminal_t* t = (terminal_t*)win->user_data;
    if (!t) return;

    for (int i = 0; i < win->buf_width * win->buf_height; i++) {
        win->buffer[i] = 0xFF300A24;
    }

    if (!g_font) {
        int content_h = win->height;
        int max_visible = (content_h / TERM_LINE_HEIGHT) - 6;
        if (max_visible < 1)
            max_visible = 1;
        int start = t->scroll_offset - max_visible;
        if (start < 0)
            start = 0;
        if (start > t->line_count - 1)
            start = t->line_count - 1;

        int y = TERM_MARGIN_Y;
        for (int i = start;
             i < start + max_visible && i < t->line_count && y < content_h;
             i++) {
            for (int py = y; py < y + TERM_LINE_HEIGHT && py < win->buf_height;
                 py++) {
                for (int px = TERM_MARGIN_X;
                     px < TERM_MARGIN_X + 200 && px < win->buf_width; px++) {
                    win->buffer[py * win->buf_width + px] = 0xFFFF0000;
                }
            }
            y += TERM_LINE_HEIGHT;
        }
        return;
    }

    int content_h = win->height;
    int max_visible = (content_h / TERM_LINE_HEIGHT) - 6;
    if (max_visible < 1)
        max_visible = 1;

    int start = t->scroll_offset - max_visible;
    if (start < 0)
        start = 0;
    if (start > t->line_count - 1)
        start = t->line_count - 1;

    int y = TERM_MARGIN_Y;

    for (int i = start;
         i < start + max_visible && i < t->line_count && y < content_h; i++) {
        ttf_draw_text_utf8_buf(g_font, TERM_MARGIN_X, y, TERM_FONT_SIZE,
                               t->colors[i], win->buffer, win->buf_width,
                               win->buf_height, t->lines[i]);
        y += TERM_LINE_HEIGHT;
    }

    if (t->line_buffer_len > 0 && y < content_h) {
        ttf_draw_text_utf8_buf(g_font, TERM_MARGIN_X, y, TERM_FONT_SIZE,
                               t->line_buffer_color, win->buffer,
                               win->buf_width, win->buf_height, t->line_buffer);
    }
}

static void term_mouse_handler(wm_window_t* win, int lx, int ly, int button) {
    (void)lx;
    (void)ly;
    if (!button || !win)
        return;

    terminal_t* t = (terminal_t*)win->user_data;
    if (t) {
        terminal_set_active(t);
    }
}

void terminal_init(void) {
    g_active_terminal = NULL;
    g_klog_term_output = NULL;
    g_terminal_initialized = false;
    kinfo("TERM", "Terminal module initialized");
}

void terminal_set_active(terminal_t* term) {
    g_active_terminal = term;
}

terminal_t* terminal_create_window(int x, int y, int width, int height) {
    terminal_t* t = (terminal_t*)kmalloc(sizeof(terminal_t));
    if (!t)
        return NULL;

    memset(t, 0, sizeof(terminal_t));
    t->line_buffer_color = 0xFFFFFF;
    for (int i = 0; i < TERM_MAX_LINES; i++) {
        t->colors[i] = 0xFFFFFF;
    }

    t->window = wm_create_window(x, y, width, height, "Terminal", term_draw,
                                 term_mouse_handler);

    if (t->window) {
        t->window->user_data = t;
        t->initialized = true;
        g_terminal_initialized = true;
        g_active_terminal = t;

        term_append_line(t, "Welcome to MWOS Shell V1.0", 0x00FF00);
        term_append_line(t, "Type 'help' for available commands", 0xAAAAAA);
        term_append_line(t, "", 0xFFFFFF);

        term_draw(t->window);
        wm_invalidate_window(t->window);
        wm_redraw_dirty();
        graphics_present();

        kinfo("TERM", "Terminal window created at %d,%d size %dx%d", x, y,
              width, height);

        return t;
    } else {
        kfree(t);
        kerror("TERM", "Failed to create terminal window");
        return NULL;
    }
}

void terminal_output(const char* str) {
    if (!str)
        return;

    terminal_t* t = g_active_terminal;
    if (!t) return;

    const char* p = str;
    uint32_t current_color = 0xFFFFFF;
    bool had_full_event = false;

    while (*p) {
        if (*p == '\033' && *(p + 1) == '[') {
            current_color = parse_ansi_color(&p);
            continue;
        }

        if (*p == '\n' || *p == '\r') {
            t->line_buffer[t->line_buffer_len] = '\0';
            term_append_line(t, t->line_buffer, t->line_buffer_color);
            t->line_buffer_len = 0;
            had_full_event = true;
            p++;
        } else if (*p == '\b') {
            if (t->line_buffer_len > 0) {
                t->line_buffer_len--;
                t->line_buffer[t->line_buffer_len] = '\0';
            } else {
                term_backspace(t);
                had_full_event = true;
            }
            p++;
        } else {
            if (t->line_buffer_len < TERM_MAX_LINE_LEN - 1) {
                t->line_buffer[t->line_buffer_len++] = *p;
                t->line_buffer[t->line_buffer_len] = '\0';
            }
            t->line_buffer_color = current_color;
            p++;
        }
    }

    wm_window_t* win = t->window;
    if (win && t->initialized) {
        if (had_full_event) {
            term_draw(win);
        } else {
            int content_h = win->height;
            int max_visible = (content_h / TERM_LINE_HEIGHT) - 6;
            if (max_visible < 1)
                max_visible = 1;

            int visible_history = t->line_count;
            if (visible_history > max_visible)
                visible_history = max_visible;
            int input_y = TERM_MARGIN_Y + visible_history * TERM_LINE_HEIGHT;

            int clear_top = input_y - 16;
            if (clear_top < 0)
                clear_top = 0;
            int clear_bottom = input_y + TERM_LINE_HEIGHT;
            if (clear_bottom > win->buf_height)
                clear_bottom = win->buf_height;

            if (clear_top < win->buf_height &&
                input_y + TERM_LINE_HEIGHT <= win->buf_height) {
                for (int py = clear_top; py < clear_bottom; py++) {
                    uint32_t* row = &win->buffer[py * win->buf_width];
                    for (int px = 0; px < win->buf_width; px++)
                        row[px] = 0xFF300A24;
                }
                if (t->line_buffer_len > 0) {
                    ttf_draw_text_utf8_buf(g_font, TERM_MARGIN_X, input_y,
                                           TERM_FONT_SIZE, t->line_buffer_color,
                                           win->buffer, win->buf_width,
                                           win->buf_height, t->line_buffer);
                }

                {
                    if (g_font && g_font->unitsPerEm != 0) {
                        int cursor_x = TERM_MARGIN_X;
                        if (t->line_buffer_len > 0)
                            cursor_x += ttf_text_width(g_font, TERM_FONT_SIZE,
                                                       t->line_buffer);

                        int asc_px = (int)((int64_t)g_font->ascender *
                                           TERM_FONT_SIZE / g_font->unitsPerEm);
                        int dsc_px = (int)((int64_t)(-g_font->descender) *
                                           TERM_FONT_SIZE / g_font->unitsPerEm);
                        int cursor_top = input_y - asc_px;
                        int cursor_h = asc_px + dsc_px;
                        int cursor_w = 3;

                        if (cursor_top < 0) {
                            cursor_h += cursor_top;
                            cursor_top = 0;
                        }
                        if (cursor_top + cursor_h > win->buf_height)
                            cursor_h = win->buf_height - cursor_top;
                        if (cursor_h < 1)
                            cursor_h = 1;

                        for (int cy = cursor_top; cy < cursor_top + cursor_h; cy++) {
                            uint32_t* row = &win->buffer[cy * win->buf_width];
                            for (int cx = cursor_x;
                                 cx < cursor_x + cursor_w && cx < win->buf_width; cx++) {
                                row[cx] = 0xFFFFFFFF;
                            }
                        }
                    }
                }
            }
        }
        wm_invalidate_window(win);
        wm_redraw_dirty();
    }
}

void terminal_clear(void) {
    terminal_t* t = g_active_terminal;
    if (!t) return;

    t->line_count = 0;
    t->scroll_offset = 0;
    t->line_buffer_len = 0;

    if (t->window) {
        term_draw(t->window);
        wm_invalidate_window(t->window);
        wm_redraw_dirty();
    }
}

void terminal_refresh(void) {
    terminal_t* t = g_active_terminal;
    if (!t) return;

    if (t->window && t->initialized) {
        term_draw(t->window);
        wm_invalidate_window(t->window);
        wm_redraw_dirty();
    }
}

void terminal_scroll(int delta) {
    terminal_t* t = g_active_terminal;
    if (!t) return;

    t->scroll_offset += delta;

    if (t->scroll_offset < 0)
        t->scroll_offset = 0;
    if (t->scroll_offset > t->line_count)
        t->scroll_offset = t->line_count;

    terminal_refresh();
}

void terminal_set_color(uint32_t color) { (void)color; }

terminal_t* terminal_get(void) { return g_active_terminal; }

wm_window_t* terminal_get_window(void) {
    if (g_active_terminal)
        return g_active_terminal->window;
    return NULL;
}
