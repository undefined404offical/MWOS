#include "wm.h"
#include "graphics.h"
#include "ttf.h"
#include "memory.h"
#include "string.h"
#include "serial.h"

extern boot_params_t *g_framebuffer;
extern TTF_Font *g_font;
extern int32_t mouse_x;
extern int32_t mouse_y;

#define CURSOR_W 16
#define CURSOR_H 24
extern const char mouse_cursor[CURSOR_H][CURSOR_W];

static int g_screen_w = 0;
static int g_screen_h = 0;

wm_window_t *g_dragging_window = NULL;
static wm_window_t *g_window_list = NULL; // 底→顶
static int g_next_window_id = 1;

static uint32_t g_mouse_bg[CURSOR_H][CURSOR_W];
int32_t g_old_mouse_x = -1;
int32_t g_old_mouse_y = -1;

extern uint32_t *g_backbuffer;
extern uint32_t g_backbuffer_pitch;

int preview_x = -1;
int preview_y = -1;
bool preview_active = false;
static bool g_xor_drag_active = false;
static int g_xor_last_x = 0;
static int g_xor_last_y = 0;
static int g_xor_last_w = 0;
static int g_xor_last_h = 0;

// 任务栏按钮
typedef struct
{
    wm_window_t *win;
    int x, y, w, h;
} taskbar_button_t;

static taskbar_button_t g_taskbar[32];
static int g_taskbar_count = 0;

// dirty rect
typedef struct
{
    int x1, y1;
    int x2, y2; // [x1,x2), [y1,y2)
    bool valid;
} wm_dirty_t;

static wm_dirty_t g_dirty = {0, 0, 0, 0, false};

// ------------------------------------------------
// dirty rect 管理
// ------------------------------------------------

static void wm_dirty_reset(void)
{
    g_dirty.valid = false;
}

void wm_invalidate_rect(int x, int y, int w, int h)
{
    int x2 = x + w;
    int y2 = y + h;

    if (!g_dirty.valid)
    {
        g_dirty.x1 = x;
        g_dirty.y1 = y;
        g_dirty.x2 = x2;
        g_dirty.y2 = y2;
        g_dirty.valid = true;
    }
    else
    {
        if (x < g_dirty.x1)
            g_dirty.x1 = x;
        if (y < g_dirty.y1)
            g_dirty.y1 = y;
        if (x2 > g_dirty.x2)
            g_dirty.x2 = x2;
        if (y2 > g_dirty.y2)
            g_dirty.y2 = y2;
    }
}

void wm_invalidate_window(wm_window_t *win)
{
    if (!win)
        return;
    int x = win->x;
    int y = win->y;
    int w = win->width;
    int h = win->title_height + win->height;
    wm_invalidate_rect(x, y, w, h);
}

// ------------------------------------------------
// 辅助：任务栏按钮
// ------------------------------------------------

static void taskbar_add_button(wm_window_t *win)
{
    // 已存在则不重复添加
    for (int i = 0; i < g_taskbar_count; i++)
    {
        if (g_taskbar[i].win == win)
        {
            return;
        }
    }
    if (g_taskbar_count >= 32)
        return;

    int index = g_taskbar_count++;
    taskbar_button_t *b = &g_taskbar[index];
    b->win = win;
    b->w = 120;
    b->h = 24;
    b->x = 4 + index * 124;
    b->y = g_screen_h - 26;
}

static void taskbar_update_positions(void)
{
    for (int i = 0; i < g_taskbar_count; i++)
    {
        g_taskbar[i].x = 4 + i * 124;
        g_taskbar[i].y = g_screen_h - 26;
    }
}

// ------------------------------------------------
// 标题栏按钮布局
// ------------------------------------------------

static void wm_update_titlebar_buttons(wm_window_t *win)
{
    win->btn_min_w = 20;
    win->btn_min_h = 20;
    win->btn_min_x = win->width - 72;
    win->btn_min_y = 4;

    win->btn_max_w = 20;
    win->btn_max_h = 20;
    win->btn_max_x = win->width - 48;
    win->btn_max_y = 4;

    win->btn_close_w = 20;
    win->btn_close_h = 20;
    win->btn_close_x = win->width - 24;
    win->btn_close_y = 4;
}

// ------------------------------------------------
// 初始化
// ------------------------------------------------

void wm_init(int screen_w, int screen_h)
{
    g_screen_w = screen_w;
    g_screen_h = screen_h;
    g_window_list = NULL;
    g_next_window_id = 1;
    g_dragging_window = NULL;
    g_taskbar_count = 0;
    wm_dirty_reset();

    serial_puts("WM: init OK\n");
    bool *klog_to_screen = false;
}

// ------------------------------------------------
// 鼠标背景保存/恢复/绘制（直接在 framebuffer 上操作）
// ------------------------------------------------

void mouse_save_bg(int mx, int my)
{
    if (!g_framebuffer)
        return;

    uint32_t *fb = (uint32_t *)g_framebuffer->framebuffer_addr;
    uint32_t pitch = g_framebuffer->framebuffer_pitch >> 2;
    int sw = g_framebuffer->framebuffer_width;
    int sh = g_framebuffer->framebuffer_height;

    for (int y = 0; y < CURSOR_H; y++)
    {
        int sy = my + y;
        if (sy < 0 || sy >= sh)
            continue;
        for (int x = 0; x < CURSOR_W; x++)
        {
            int sx = mx + x;
            if (sx < 0 || sx >= sw)
                continue;
            g_mouse_bg[y][x] = fb[sy * pitch + sx];
        }
    }
}

void mouse_restore_bg(int mx, int my)
{
    if (!g_framebuffer)
        return;
 
    uint32_t *fb = (uint32_t *)g_framebuffer->framebuffer_addr;
    uint32_t pitch = g_framebuffer->framebuffer_pitch >> 2;
    int sw = g_framebuffer->framebuffer_width;
    int sh = g_framebuffer->framebuffer_height;

    for (int y = 0; y < CURSOR_H; y++)
    {
        int sy = my + y;
        if (sy < 0 || sy >= sh)
            continue;
        for (int x = 0; x < CURSOR_W; x++)
        {
            int sx = mx + x;
            if (sx < 0 || sx >= sw)
                continue;
            fb[sy * pitch + sx] = g_mouse_bg[y][x];
        }
    }
}

void mouse_draw(int mx, int my)
{
    if (!g_framebuffer)
        return;

    uint32_t *fb = (uint32_t *)g_framebuffer->framebuffer_addr;
    uint32_t pitch = g_framebuffer->framebuffer_pitch >> 2;
    int sw = g_framebuffer->framebuffer_width;
    int sh = g_framebuffer->framebuffer_height;

    for (int y = 0; y < CURSOR_H; y++)
    {
        int sy = my + y;
        if (sy < 0 || sy >= sh)
            continue;
        for (int x = 0; x < CURSOR_W; x++)
        {
            int sx = mx + x;
            if (sx < 0 || sx >= sw)
                continue;

            char p = mouse_cursor[y][x];
            if (p == 'X')
                fb[sy * pitch + sx] = 0xFF000000;
            else if (p == '.')
                fb[sy * pitch + sx] = 0xFFFFFFFF;
        }
    }
}

// ------------------------------------------------
// 窗口创建、关闭、缓冲区调整
// ------------------------------------------------

wm_window_t *wm_create_window(int x, int y, int w, int h,
                              const char *title,
                              wm_draw_callback_t draw_func,
                              wm_event_callback_t event_func)
{
    wm_window_t *win = (wm_window_t *)kmalloc(sizeof(wm_window_t));
    if (!win)
        return NULL;

    memset(win, 0, sizeof(*win));

    win->id = g_next_window_id++;
    win->x = x;
    win->y = y;
    win->width = w;
    win->height = h;
    win->title_height = 28;

    if (title)
    {
        strncpy(win->title, title, sizeof(win->title) - 1);
        win->title[sizeof(win->title) - 1] = '\0';
    }

    win->buffer = (uint32_t *)kmalloc(sizeof(uint32_t) * w * h);
    if (!win->buffer)
    {
        kfree(win);
        return NULL;
    }
    // 初始化窗口缓冲区为浅灰色背景，便于显示文字
    for (int i = 0; i < w * h; i++) {
        win->buffer[i] = 0xFF202020; // 浅灰色背景
    }

    win->buf_width = w;
    win->buf_height = h;

    win->visible = true;
    win->minimized = false;
    win->maximized = false;
    win->draw = draw_func;
    win->on_mouse = event_func;

    wm_update_titlebar_buttons(win);

    // 插入窗口链表（尾部）
    if (!g_window_list)
    {
        g_window_list = win;
    }
    else
    {
        wm_window_t *cur = g_window_list;
        while (cur->next)
            cur = cur->next;
        cur->next = win;
    }

    taskbar_add_button(win);

    if (win->draw)
    {
        win->draw(win);
    }

    serial_puts("WM: before first draw\n");
    if (win->draw)
    {
        serial_puts("WM: draw ptr = ");
        serial_putptr(win->draw);
        serial_puts("\n");
        win->draw(win);
    }
    serial_puts("WM: after first draw\n");

    wm_invalidate_window(win);

    serial_puts("WM: created window\n");
    return win;
}

void wm_resize_window_buffer(wm_window_t *win, int new_w, int new_h)
{
    int old_w = win->buf_width;
    int old_h = win->buf_height;

    uint32_t *newbuf = kmalloc(new_w * new_h * 4);
    if (!newbuf)
    {
        serial_puts("wm: resize failed, out of memory\n");
        return;
    }

    memset(newbuf, 0x20, new_w * new_h * 4);

    int copy_w = (new_w < old_w) ? new_w : old_w;
    int copy_h = (new_h < old_h) ? new_h : old_h;

    for (int y = 0; y < copy_h; y++)
    {
        memcpy(&newbuf[y * new_w], &win->buffer[y * old_w], copy_w * 4);
    }

    kfree(win->buffer);
    win->buffer = newbuf;
    win->buf_width = new_w;
    win->buf_height = new_h;
}

void wm_close_window(wm_window_t *win)
{
    if (!win)
        return;

    // 从窗口链表删除
    if (g_window_list == win)
    {
        g_window_list = win->next;
    }
    else
    {
        wm_window_t *cur = g_window_list;
        while (cur && cur->next != win)
            cur = cur->next;
        if (cur)
            cur->next = win->next;
    }

    // 从任务栏删除
    for (int i = 0; i < g_taskbar_count; i++)
    {
        if (g_taskbar[i].win == win)
        {
            // 标记任务栏按钮区域为无效
            wm_invalidate_rect(g_taskbar[i].x, g_taskbar[i].y, g_taskbar[i].w, g_taskbar[i].h);
            
            for (int j = i; j < g_taskbar_count - 1; j++)
            {
                g_taskbar[j] = g_taskbar[j + 1];
            }
            g_taskbar_count--;
            taskbar_update_positions();
            break;
        }
    }

    // 失效区域
    wm_invalidate_window(win);

    // 释放窗口缓冲区
    if (win->buffer)
        kfree(win->buffer);

    serial_puts("WM: window closed\n");

    kfree(win);
}

// ------------------------------------------------
// 命中检测：从顶到底找窗口
// ------------------------------------------------

wm_window_t *wm_pick_window_at(int x, int y)
{
    if (!g_window_list)
        return NULL;

    wm_window_t *stack[128];
    int count = 0;
    for (wm_window_t *w = g_window_list; w && count < 128; w = w->next)
    {
        stack[count++] = w;
    }

    for (int i = count - 1; i >= 0; i--)
    {
        wm_window_t *w = stack[i];
        int tx = w->x;
        int ty = w->y;
        int th = w->title_height;
        int total_h = th + w->height;

        if (!w->visible || w->minimized)
            continue;

        if (x >= tx && x < tx + w->width &&
            y >= ty && y < ty + total_h)
        {
            return w;
        }
    }

    return NULL;
}

// ------------------------------------------------
// 绘制：单个窗口到 backbuffer（不裁剪）
// ------------------------------------------------

static void wm_draw_single_window_to(uint32_t *buf, uint32_t pitch, wm_window_t *win)
{
    if (!buf || !g_framebuffer || !win)
        return;

    int sw = g_framebuffer->framebuffer_width;
    int sh = g_framebuffer->framebuffer_height;

    int tx = win->x;
    int ty = win->y;
    int w = win->width;
    int h = win->height;
    int th = win->title_height;

    // 标题栏背景
    for (int yy = 0; yy < th; yy++)
    {
        int sy = ty + yy;
        if (sy < 0 || sy >= sh)
            continue;
        for (int xx = 0; xx < w; xx++)
        {
            int sx = tx + xx;
            if (sx < 0 || sx >= sw)
                continue;
            buf[sy * pitch + sx] = 0xFF224488;
        }
    }

    // 标题文字
    if (g_font)
    {
        ttf_draw_text_utf8(
            g_font,
            win->x + 8,
            win->y + 20,
            20,
            0xFFFFFFFF,
            win->title);
    }

    // 最小化按钮
    for (int yy = 0; yy < win->btn_min_h; yy++)
    {
        for (int xx = 0; xx < win->btn_min_w; xx++)
        {
            int sx = win->x + win->btn_min_x + xx;
            int sy = win->y + win->btn_min_y + yy;
            if (sx >= 0 && sx < sw && sy >= 0 && sy < sh)
                buf[sy * pitch + sx] = 0xFF666666;
        }
    }
    for (int xx = 4; xx < 16; xx++)
    {
        int sx = win->x + win->btn_min_x + xx;
        int sy = win->y + win->btn_min_y + 14;
        if (sx >= 0 && sx < sw && sy >= 0 && sy < sh)
            buf[sy * pitch + sx] = 0xFFFFFFFF;
    }

    // 最大化按钮
    for (int yy = 0; yy < win->btn_max_h; yy++)
    {
        for (int xx = 0; xx < win->btn_max_w; xx++)
        {
            int sx = win->x + win->btn_max_x + xx;
            int sy = win->y + win->btn_max_y + yy;
            if (sx >= 0 && sx < sw && sy >= 0 && sy < sh)
                buf[sy * pitch + sx] = 0xFF666666;
        }
    }
    for (int xx = 4; xx < 16; xx++)
    {
        int sx1 = win->x + win->btn_max_x + xx;
        int sy1 = win->y + win->btn_max_y + 4;
        int sy2 = win->y + win->btn_max_y + 16;

        if (sx1 >= 0 && sx1 < sw)
        {
            if (sy1 >= 0 && sy1 < sh)
                buf[sy1 * pitch + sx1] = 0xFFFFFFFF;
            if (sy2 >= 0 && sy2 < sh)
                buf[sy2 * pitch + sx1] = 0xFFFFFFFF;
        }
    }
    for (int yy = 4; yy < 16; yy++)
    {
        int sy = win->y + win->btn_max_y + yy;
        int sx1 = win->x + win->btn_max_x + 4;
        int sx2 = win->x + win->btn_max_x + 16;

        if (sy >= 0 && sy < sh)
        {
            if (sx1 >= 0 && sx1 < sw)
                buf[sy * pitch + sx1] = 0xFFFFFFFF;
            if (sx2 >= 0 && sx2 < sw)
                buf[sy * pitch + sx2] = 0xFFFFFFFF;
        }
    }

    // 关闭按钮
    for (int yy = 0; yy < win->btn_close_h; yy++)
    {
        for (int xx = 0; xx < win->btn_close_w; xx++)
        {
            int sx = win->x + win->btn_close_x + xx;
            int sy = win->y + win->btn_close_y + yy;
            if (sx >= 0 && sx < sw && sy >= 0 && sy < sh)
                buf[sy * pitch + sx] = 0xFFAA3333;
        }
    }
    for (int i = 4; i < 16; i++)
    {
        int sx1 = win->x + win->btn_close_x + i;
        int sy1 = win->y + win->btn_close_y + i;
        int sx2 = win->x + win->btn_close_x + (16 - (i - 4));
        int sy2 = win->y + win->btn_close_y + i;

        if (sx1 >= 0 && sx1 < sw && sy1 >= 0 && sy1 < sh)
            buf[sy1 * pitch + sx1] = 0xFFFFFFFF;

        if (sx2 >= 0 && sx2 < sw && sy2 >= 0 && sy2 < sh)
            buf[sy2 * pitch + sx2] = 0xFFFFFFFF;
    }

    // 边框
    int bx0 = tx;
    int bx1 = tx + w - 1;
    int by0 = ty;
    int by1 = ty + th + h - 1;

    if (by0 >= 0 && by0 < sh)
    {
        for (int x = bx0; x <= bx1; x++)
        {
            if (x >= 0 && x < sw)
                buf[by0 * pitch + x] = 0xFF000000;
        }
    }
    if (by1 >= 0 && by1 < sh)
    {
        for (int x = bx0; x <= bx1; x++)
        {
            if (x >= 0 && x < sw)
                buf[by1 * pitch + x] = 0xFF000000;
        }
    }
    for (int y = by0; y <= by1; y++)
    {
        if (y >= 0 && y < sh)
        {
            if (bx0 >= 0 && bx0 < sw)
                buf[y * pitch + bx0] = 0xFF000000;
            if (bx1 >= 0 && bx1 < sw)
                buf[y * pitch + bx1] = 0xFF000000;
        }
    }

    // 内容区
    int bw = win->buf_width;
    int bh = win->buf_height;

    for (int yy = 0; yy < bh; yy++)
    {
        int sy = ty + th + yy;
        if (sy < 0 || sy >= sh)
            continue;
        for (int xx = 0; xx < bw; xx++)
        {
            int sx = tx + xx;
            if (sx < 0 || sx >= sw)
                continue;
            buf[sy * pitch + sx] = win->buffer[yy * bw + xx];
        }
    }
}

// 带裁剪版本，用于 dirty rect
static void wm_draw_window_clipped_to(uint32_t *buf, uint32_t pitch,
                                      wm_window_t *win,
                                      int clip_x1, int clip_y1,
                                      int clip_x2, int clip_y2)
{
    if (!buf || !g_framebuffer || !win)
        return;

    int sw = g_framebuffer->framebuffer_width;
    int sh = g_framebuffer->framebuffer_height;

    int tx = win->x;
    int ty = win->y;
    int w = win->width;
    int h = win->height;
    int th = win->title_height;

    int wx1 = tx;
    int wy1 = ty;
    int wx2 = tx + w;
    int wy2 = ty + th + h;

    // 窗口整体和屏幕无交集
    if (wx1 >= sw || wy1 >= sh || wx2 <= 0 || wy2 <= 0)
        return;

    // 和 clip 求交集
    int rx1 = (wx1 > clip_x1) ? wx1 : clip_x1;
    int ry1 = (wy1 > clip_y1) ? wy1 : clip_y1;
    int rx2 = (wx2 < clip_x2) ? wx2 : clip_x2;
    int ry2 = (wy2 < clip_y2) ? wy2 : clip_y2;
    if (rx1 >= rx2 || ry1 >= ry2)
        return;

    // 标题栏背景
    for (int y = ty; y < ty + th; y++)
    {
        if (y < ry1 || y >= ry2)
            continue;
        if (y < 0 || y >= sh)
            continue;
        for (int x = tx; x < tx + w; x++)
        {
            if (x < rx1 || x >= rx2)
                continue;
            if (x < 0 || x >= sw)
                continue;
            buf[y * pitch + x] = 0xFF224488;
        }
    }

    // 标题文字（不做严格裁剪，简单判断在 clip 内就画）
    if (g_font)
    {
        int text_x = win->x + 8;
        int text_y = win->y + 20;
        if (text_x < clip_x2 && text_x >= clip_x1 &&
            text_y < clip_y2 && text_y >= clip_y1)
        {
            ttf_draw_text_utf8(
                g_font,
                text_x,
                text_y,
                20,
                0xFFFFFFFF,
                win->title);
        }
    }

    int sw_w = sw; // just alias

    // 最小化按钮
    for (int yy = 0; yy < win->btn_min_h; yy++)
    {
        for (int xx = 0; xx < win->btn_min_w; xx++)
        {
            int sx = win->x + win->btn_min_x + xx;
            int sy = win->y + win->btn_min_y + yy;
            if (sx < rx1 || sx >= rx2 || sy < ry1 || sy >= ry2)
                continue;
            if (sx >= 0 && sx < sw_w && sy >= 0 && sy < sh)
                buf[sy * pitch + sx] = 0xFF666666;
        }
    }
    for (int xx = 4; xx < 16; xx++)
    {
        int sx = win->x + win->btn_min_x + xx;
        int sy = win->y + win->btn_min_y + 14;
        if (sx < rx1 || sx >= rx2 || sy < ry1 || sy >= ry2)
            continue;
        if (sx >= 0 && sx < sw_w && sy >= 0 && sy < sh)
            buf[sy * pitch + sx] = 0xFFFFFFFF;
    }

    // 最大化按钮
    for (int yy = 0; yy < win->btn_max_h; yy++)
    {
        for (int xx = 0; xx < win->btn_max_w; xx++)
        {
            int sx = win->x + win->btn_max_x + xx;
            int sy = win->y + win->btn_max_y + yy;
            if (sx < rx1 || sx >= rx2 || sy < ry1 || sy >= ry2)
                continue;
            if (sx >= 0 && sx < sw_w && sy >= 0 && sy < sh)
                buf[sy * pitch + sx] = 0xFF666666;
        }
    }
    for (int xx = 4; xx < 16; xx++)
    {
        int sx1 = win->x + win->btn_max_x + xx;
        int sy1 = win->y + win->btn_max_y + 4;
        int sy2 = win->y + win->btn_max_y + 16;

        if (sx1 < rx1 || sx1 >= rx2)
            continue;

        if (sy1 >= ry1 && sy1 < ry2 && sy1 >= 0 && sy1 < sh)
            buf[sy1 * pitch + sx1] = 0xFFFFFFFF;
        if (sy2 >= ry1 && sy2 < ry2 && sy2 >= 0 && sy2 < sh)
            buf[sy2 * pitch + sx1] = 0xFFFFFFFF;
    }
    for (int yy = 4; yy < 16; yy++)
    {
        int sy = win->y + win->btn_max_y + yy;
        int sx1 = win->x + win->btn_max_x + 4;
        int sx2 = win->x + win->btn_max_x + 16;

        if (sy < ry1 || sy >= ry2 || sy < 0 || sy >= sh)
            continue;
        if (sx1 >= rx1 && sx1 < rx2 && sx1 >= 0 && sx1 < sw_w)
            buf[sy * pitch + sx1] = 0xFFFFFFFF;
        if (sx2 >= rx1 && sx2 < rx2 && sx2 >= 0 && sx2 < sw_w)
            buf[sy * pitch + sx2] = 0xFFFFFFFF;
    }

    // 关闭按钮
    for (int yy = 0; yy < win->btn_close_h; yy++)
    {
        for (int xx = 0; xx < win->btn_close_w; xx++)
        {
            int sx = win->x + win->btn_close_x + xx;
            int sy = win->y + win->btn_close_y + yy;
            if (sx < rx1 || sx >= rx2 || sy < ry1 || sy >= ry2)
                continue;
            if (sx >= 0 && sx < sw_w && sy >= 0 && sy < sh)
                buf[sy * pitch + sx] = 0xFFAA3333;
        }
    }
    for (int i = 4; i < 16; i++)
    {
        int sx1 = win->x + win->btn_close_x + i;
        int sy1 = win->y + win->btn_close_y + i;
        int sx2 = win->x + win->btn_close_x + (16 - (i - 4));
        int sy2 = win->y + win->btn_close_y + i;

        if (sx1 >= rx1 && sx1 < rx2 && sy1 >= ry1 && sy1 < ry2 &&
            sx1 >= 0 && sx1 < sw_w && sy1 >= 0 && sy1 < sh)
        {
            buf[sy1 * pitch + sx1] = 0xFFFFFFFF;
        }
        if (sx2 >= rx1 && sx2 < rx2 && sy2 >= ry1 && sy2 < ry2 &&
            sx2 >= 0 && sx2 < sw_w && sy2 >= 0 && sy2 < sh)
        {
            buf[sy2 * pitch + sx2] = 0xFFFFFFFF;
        }
    }

    // 边框
    int bx0 = tx;
    int bx1 = tx + w - 1;
    int by0 = ty;
    int by1 = ty + th + h - 1;

    if (by0 >= ry1 && by0 < ry2 && by0 >= 0 && by0 < sh)
    {
        for (int x = bx0; x <= bx1; x++)
        {
            if (x < rx1 || x >= rx2)
                continue;
            if (x >= 0 && x < sw_w)
                buf[by0 * pitch + x] = 0xFF000000;
        }
    }
    if (by1 >= ry1 && by1 < ry2 && by1 >= 0 && by1 < sh)
    {
        for (int x = bx0; x <= bx1; x++)
        {
            if (x < rx1 || x >= rx2)
                continue;
            if (x >= 0 && x < sw_w)
                buf[by1 * pitch + x] = 0xFF000000;
        }
    }
    for (int y = by0; y <= by1; y++)
    {
        if (y < ry1 || y >= ry2 || y < 0 || y >= sh)
            continue;
        if (bx0 >= rx1 && bx0 < rx2 && bx0 >= 0 && bx0 < sw_w)
            buf[y * pitch + bx0] = 0xFF000000;
        if (bx1 >= rx1 && bx1 < rx2 && bx1 >= 0 && bx1 < sw_w)
            buf[y * pitch + bx1] = 0xFF000000;
    }

    // 内容区
    int bw = win->buf_width;
    int bh = win->buf_height;

    for (int yy = 0; yy < bh; yy++)
    {
        int sy = ty + th + yy;
        if (sy < ry1 || sy >= ry2 || sy < 0 || sy >= sh)
            continue;

        for (int xx = 0; xx < bw; xx++)
        {
            int sx = tx + xx;
            if (sx < rx1 || sx >= rx2 || sx < 0 || sx >= sw_w)
                continue;

            buf[sy * pitch + sx] = win->buffer[yy * bw + xx];
        }
    }
}

// ------------------------------------------------
// 重绘：dirty rect 版本
// ------------------------------------------------

void wm_redraw_dirty(void)
{
    if (!g_framebuffer || !g_backbuffer || !g_dirty.valid)
        return;

    uint32_t *fb = (uint32_t *)g_framebuffer->framebuffer_addr;
    uint32_t fb_p = g_framebuffer->framebuffer_pitch >> 2;
    int sw = g_framebuffer->framebuffer_width;
    int sh = g_framebuffer->framebuffer_height;

    uint32_t *bb = g_backbuffer;
    uint32_t bb_p = g_backbuffer_pitch;

    int x1 = g_dirty.x1;
    int y1 = g_dirty.y1;
    int x2 = g_dirty.x2;
    int y2 = g_dirty.y2;

    if (x1 < 0)
        x1 = 0;
    if (y1 < 0)
        y1 = 0;
    if (x2 > sw)
        x2 = sw;
    if (y2 > sh)
        y2 = sh;

    if (x1 >= x2 || y1 >= y2)
    {
        wm_dirty_reset();
        return;
    }

    // 1) 先把 dirty 区域的 backbuffer 清为桌面色
    for (int y = y1; y < y2; y++)
    {
        if (y < 0 || y >= sh)
            continue;
        uint32_t *row = &bb[y * bb_p];
        for (int x = x1; x < x2; x++)
        {
            if (x < 0 || x >= sw)
                continue;
            row[x] = 0xFF169DE2;
        }
    }

    // 2) 从底到顶，重画和 dirty 相交的窗口
    for (wm_window_t *win = g_window_list; win; win = win->next)
    {
        if (!win->visible || win->minimized)
            continue;
        wm_draw_window_clipped_to(bb, bb_p, win, x1, y1, x2, y2);
    }

    // 3) 画任务栏（即使部分在 dirty 区域，也按裁剪画）
    for (int x = x1; x < x2; x++)
    {
        for (int y = g_screen_h - 28; y < g_screen_h; y++)
        {
            if (y < y1 || y >= y2)
                continue;
            if (x < 0 || x >= sw || y < 0 || y >= sh)
                continue;
            bb[y * bb_p + x] = 0xFF303030;
        }
    }

    for (int i = 0; i < g_taskbar_count; i++)
    {
        taskbar_button_t *b = &g_taskbar[i];
        for (int yy = 0; yy < b->h; yy++)
        {
            int sy = b->y + yy;
            if (sy < y1 || sy >= y2)
                continue;
            if (sy < 0 || sy >= sh)
                continue;
            for (int xx = 0; xx < b->w; xx++)
            {
                int sx = b->x + xx;
                if (sx < x1 || sx >= x2)
                    continue;
                if (sx < 0 || sx >= sw)
                    continue;
                bb[sy * bb_p + sx] = 0xFF505050;
            }
        }
        if (g_font)
        {
            int tx = b->x + 6;
            int ty = b->y + 19;
            if (tx < x2 && tx >= x1 && ty < y2 && ty >= y1)
            {
                ttf_draw_text_utf8(
                    g_font,
                    tx,
                    ty,
                    18,
                    0xFFFFFFFF,
                    b->win->title);
            }
        }
    }

    // 4) 把 dirty 区域从 backbuffer 拷到 framebuffer
    for (int y = y1; y < y2; y++)
    {
        if (y < 0 || y >= sh)
            continue;
        uint32_t *src = &bb[y * bb_p];
        uint32_t *dst = &fb[y * fb_p];
        for (int x = x1; x < x2; x++)
        {
            if (x < 0 || x >= sw)
                continue;
            dst[x] = src[x];
        }
    }
    // mouse_draw(mouse_x, mouse_y);

    wm_dirty_reset();
}

// 兼容旧接口：全屏重绘 → 实际用 dirty rect 完成
void wm_redraw(void)
{
    wm_invalidate_rect(0, 0, g_screen_w, g_screen_h);
    wm_redraw_dirty();
}

// ------------------------------------------------
// 鼠标事件处理
// ------------------------------------------------

static void wm_draw_xor_rect_fb(int x, int y, int w, int h)
{
    if (!g_framebuffer)
        return;

    uint32_t *fb = (uint32_t *)g_framebuffer->framebuffer_addr;
    uint32_t pitch = g_framebuffer->framebuffer_pitch >> 2;
    int sw = g_framebuffer->framebuffer_width;
    int sh = g_framebuffer->framebuffer_height;

    int x1 = x;
    int y1 = y;
    int x2 = x + w;
    int y2 = y + h;

    if (x1 < 0)
        x1 = 0;
    if (y1 < 0)
        y1 = 0;
    if (x2 > sw)
        x2 = sw;
    if (y2 > sh)
        y2 = sh;
    if (x1 >= x2 || y1 >= y2)
        return;

    // 上边
    for (int xx = x1; xx < x2; xx++)
    {
        uint32_t *p = &fb[y1 * pitch + xx];
        *p ^= 0x00FFFFFF;
    }
    // 下边
    if (y2 - 1 >= y1 && y2 - 1 < sh)
    {
        for (int xx = x1; xx < x2; xx++)
        {
            uint32_t *p = &fb[(y2 - 1) * pitch + xx];
            *p ^= 0x00FFFFFF;
        }
    }
    // 左边
    for (int yy = y1; yy < y2; yy++)
    {
        uint32_t *p = &fb[yy * pitch + x1];
        *p ^= 0x00FFFFFF;
    }
    // 右边
    if (x2 - 1 >= x1 && x2 - 1 < sw)
    {
        for (int yy = y1; yy < y2; yy++)
        {
            uint32_t *p = &fb[yy * pitch + (x2 - 1)];
            *p ^= 0x00FFFFFF;
        }
    }
}

bool wm_handle_mouse(int x, int y, bool left_down)
{
    static bool prev_left_down = false;
    bool window_moved = false;

    // 1. 鼠标刚按下
    if (left_down && !prev_left_down)
    {
        // 1.1 先检查任务栏按钮
        for (int i = 0; i < g_taskbar_count; i++)
        {
            taskbar_button_t *b = &g_taskbar[i];
            if (x >= b->x && x < b->x + b->w &&
                y >= b->y && y < b->y + b->h)
            {

                b->win->visible = true;
                b->win->minimized = false;

                wm_invalidate_window(b->win);
                wm_redraw_dirty();

                prev_left_down = left_down;
                return false;
            }
        }

        // 1.2 检查是否点到窗口
        wm_window_t *hit = wm_pick_window_at(x, y);
        if (hit)
        {
            int lx = x - hit->x;
            int ly = y - hit->y;

            hit->on_mouse(hit, lx, ly, left_down);

            // 点击最小化按钮
            if (lx >= hit->btn_min_x && lx < hit->btn_min_x + hit->btn_min_w &&
                ly >= hit->btn_min_y && ly < hit->btn_min_y + hit->btn_min_h)
            {

                hit->visible = false;
                hit->minimized = true;
                taskbar_add_button(hit);

                wm_invalidate_window(hit);
                wm_redraw_dirty();

                prev_left_down = left_down;
                return false;
            }

            // 点击最大化按钮
            if (lx >= hit->btn_max_x && lx < hit->btn_max_x + hit->btn_max_w &&
                ly >= hit->btn_max_y && ly < hit->btn_max_y + hit->btn_max_h)
            {

                if (!hit->maximized)
                {
                    hit->restore_x = hit->x;
                    hit->restore_y = hit->y;
                    hit->restore_w = hit->width;
                    hit->restore_h = hit->height;

                    hit->x = 0;
                    hit->y = 0;
                    hit->width = g_screen_w;
                    hit->height = g_screen_h - hit->title_height - 28;
                    hit->maximized = true;
                    hit->minimized = false;

                    wm_resize_window_buffer(hit, hit->width, hit->height);
                    wm_update_titlebar_buttons(hit);
                    if (hit->draw)
                        hit->draw(hit);
                }
                else
                {
                    // 保存旧位置用于重绘背景
                    int old_x = hit->x;
                    int old_y = hit->y;
                    int old_w = hit->width;
                    int old_h = hit->title_height + hit->height;

                    hit->x = hit->restore_x;
                    hit->y = hit->restore_y;
                    hit->width = hit->restore_w;
                    hit->height = hit->restore_h;
                    hit->maximized = false;
                    hit->minimized = false;

                    wm_resize_window_buffer(hit, hit->width, hit->height);
                    wm_update_titlebar_buttons(hit);
                    if (hit->draw)
                        hit->draw(hit);

                    // 重绘窗口新位置和旧位置（修复背景）
                    wm_invalidate_window(hit);
                    wm_invalidate_rect(old_x, old_y, old_w, old_h);
                }

                wm_redraw_dirty();

                prev_left_down = left_down;
                return false;
            }

            // 点击关闭按钮
            if (lx >= hit->btn_close_x && lx < hit->btn_close_x + hit->btn_close_w &&
                ly >= hit->btn_close_y && ly < hit->btn_close_y + hit->btn_close_h)
            {

                wm_invalidate_window(hit);
                wm_close_window(hit);
                wm_redraw_dirty();

                prev_left_down = left_down;
                return false;
            }

            // 标题栏拖动开始：只画 XOR 框，不移动窗口
            // 全屏窗口不允许拖动
            if (y >= hit->y && y < hit->y + hit->title_height && !hit->maximized)
            {
                g_dragging_window = hit;
                hit->is_moving = true;
                hit->drag_offset_x = x - hit->x;
                hit->drag_offset_y = y - hit->y;

                preview_x = hit->x;
                preview_y = hit->y;
                preview_active = true;

                g_xor_drag_active = true;
                g_xor_last_x = hit->x;
                g_xor_last_y = hit->y;
                g_xor_last_w = hit->width;
                g_xor_last_h = hit->title_height + hit->height;

                // 画第一次 XOR 框（原始位置）
                wm_draw_xor_rect_fb(g_xor_last_x, g_xor_last_y,
                                    g_xor_last_w, g_xor_last_h);

                prev_left_down = left_down;
                return false;
            }

            // 内容区事件回调
            if (hit->on_mouse)
            {
                int content_lx = lx;
                int content_ly = ly - hit->title_height;
                if (content_ly >= 0)
                {
                    hit->on_mouse(hit, content_lx, content_ly, left_down);
                    wm_invalidate_window(hit);
                    wm_redraw_dirty();
                    prev_left_down = left_down;
                    return false;
                }
            }
        }
    }

    // 2. 鼠标按住移动（拖动预览，仅动 XOR 框）
    if (left_down && g_dragging_window && g_dragging_window->is_moving && g_xor_drag_active)
    {
        wm_window_t *win = g_dragging_window;

        int new_x = x - win->drag_offset_x;
        int new_y = y - win->drag_offset_y;
        int new_w = win->width;
        int new_h = win->title_height + win->height;

        if (new_x != g_xor_last_x || new_y != g_xor_last_y)
        {
            // 擦掉旧 XOR 框
            wm_draw_xor_rect_fb(g_xor_last_x, g_xor_last_y,
                                g_xor_last_w, g_xor_last_h);

            // 更新位置，画新 XOR 框
            g_xor_last_x = new_x;
            g_xor_last_y = new_y;
            g_xor_last_w = new_w;
            g_xor_last_h = new_h;

            wm_draw_xor_rect_fb(g_xor_last_x, g_xor_last_y,
                                g_xor_last_w, g_xor_last_h);
        }

        prev_left_down = left_down;
        return false; // 不重绘 WM
    }

    // 3. 鼠标松开（结束拖动，应用移动）
    if (!left_down && prev_left_down)
    {
        if (g_dragging_window && preview_active)
        {
            wm_window_t *win = g_dragging_window;

            if (g_xor_drag_active)
            {
                // 擦掉最后一次 XOR 框
                wm_draw_xor_rect_fb(g_xor_last_x, g_xor_last_y,
                                    g_xor_last_w, g_xor_last_h);
                g_xor_drag_active = false;
            }

            // 旧位置 dirty
            wm_invalidate_window(win);

            // 更新窗口真正位置
            win->x = g_xor_last_x;
            win->y = g_xor_last_y;

            // 新位置 dirty
            wm_invalidate_window(win);

            window_moved = true;
        }

        preview_active = false;
        if (g_dragging_window)
            g_dragging_window->is_moving = false;
        g_dragging_window = NULL;
    }

    prev_left_down = left_down;

    return window_moved;
}
