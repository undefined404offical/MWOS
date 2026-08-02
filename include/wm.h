#ifndef WM_H
#define WM_H

#include "graphics.h"
#include "kernelcb.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct window wm_window_t;

typedef void (*wm_draw_callback_t)(wm_window_t* win);
typedef void (*wm_event_callback_t)(wm_window_t* win, int lx, int ly,
                                    int button);

struct window {
    int x, y;
    int width, height;
    int title_height;
    char title[64];
    int id;

    int buf_width;
    int buf_height;

    int btn_close_x, btn_close_y;
    int btn_close_w, btn_close_h;

    int btn_min_x, btn_min_y, btn_min_w, btn_min_h;
    int btn_max_x, btn_max_y, btn_max_w, btn_max_h;

    bool visible;
    bool minimized;
    bool maximized;
    bool is_moving;
    int drag_offset_x;
    int drag_offset_y;

    int restore_x, restore_y;
    int restore_w, restore_h;

    int layer;
    bool layer_locked;

    uint32_t* buffer;

    wm_draw_callback_t draw;
    wm_event_callback_t on_mouse;

    void* user_data;

    struct window* parent;
    struct window* next;
};

extern boot_params_t* g_framebuffer;
extern int32_t mouse_x;
extern int32_t mouse_y;
extern int32_t g_old_mouse_x;
extern int32_t g_old_mouse_y;

#define CURSOR_W 16
#define CURSOR_H 24
extern const char mouse_cursor[CURSOR_H][CURSOR_W];

extern int preview_x;
extern int preview_y;
extern bool preview_active;

extern wm_window_t* g_dragging_window;

extern uint32_t* g_backbuffer;
extern uint32_t g_backbuffer_pitch;

extern wm_window_t* g_desktop_window;

void wm_init(int screen_w, int screen_h);
void wm_desktop_init(void);

wm_window_t* wm_create_window(int x, int y, int w, int h, const char* title,
                              wm_draw_callback_t draw_func,
                              wm_event_callback_t event_func);

void wm_close_window(wm_window_t* win);

void wm_set_window_layer(wm_window_t* win, int layer);
void wm_lock_window_layer(wm_window_t* win, bool lock);
void wm_bring_to_front(wm_window_t* win);

bool wm_handle_mouse(int x, int y, bool left_down);

void mouse_save_bg(int mx, int my);
void mouse_restore_bg(int mx, int my);
void mouse_draw(int mx, int my);

void wm_resize_window_buffer(wm_window_t* win, int new_w, int new_h);

void wm_redraw(void);
wm_window_t* wm_pick_window_at(int x, int y);
void wm_invalidate_rect(int x, int y, int w, int h);
void wm_invalidate_window(wm_window_t* win);
void wm_redraw_dirty(void);

#endif
