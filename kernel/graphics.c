#include "graphics.h"
#include "serial.h"
#include "memory.h"

boot_params_t *g_framebuffer = NULL;
uint32_t *g_backbuffer = NULL;
uint32_t g_backbuffer_pitch = 0; // 以像素为单位

extern bool *klog_to_screen;

void graphics_init(boot_params_t *fb_info)
{
    g_framebuffer = fb_info;

    uint32_t w = g_framebuffer->framebuffer_width;
    uint32_t h = g_framebuffer->framebuffer_height;

    g_backbuffer_pitch = w; // backbuffer 用紧密布局：pitch = width

    // 分配 w * h 像素，和 pitch 一致
    g_backbuffer = kmalloc((uint64_t)g_backbuffer_pitch *
                           (uint64_t)h *
                           sizeof(uint32_t));
    serial_puts("Allocating backbuffer: ");
    serial_putdec64(w);
    serial_puts("x");
    serial_putdec64(h);
    serial_puts(" pitch=");
    serial_putdec64(g_backbuffer_pitch);
    serial_puts("\n");
    *klog_to_screen = true;
}

void graphics_present(void)
{
    if (!g_framebuffer || !g_backbuffer)
        return;

    uint32_t *fb = (uint32_t *)g_framebuffer->framebuffer_addr;
    uint32_t fb_pitch = g_framebuffer->framebuffer_pitch >> 2;

    uint32_t w = g_framebuffer->framebuffer_width;
    uint32_t h = g_framebuffer->framebuffer_height;

    for (uint32_t y = 0; y < h; y++)
    {
        uint32_t *src = &g_backbuffer[y * g_backbuffer_pitch];
        uint32_t *dst = &fb[y * fb_pitch];
        memcpy(dst, src, w * sizeof(uint32_t));
    }
}

/* ===========================
 * 基础像素操作
 * =========================== */

void put_pixel(uint32_t x, uint32_t y, uint32_t color)
{
    if (!g_framebuffer || !g_backbuffer)
        return;
    uint32_t w = g_framebuffer->framebuffer_width;
    uint32_t h = g_framebuffer->framebuffer_height;

    if (x >= w || y >= h)
        return;

    g_backbuffer[y * g_backbuffer_pitch + x] = color;
}

uint32_t get_pixel(int x, int y)
{
    if (!g_framebuffer || !g_backbuffer)
        return 0;

    if (x < 0 || y < 0 ||
        x >= (int)g_framebuffer->framebuffer_width ||
        y >= (int)g_framebuffer->framebuffer_height)
        return 0;

    return g_backbuffer[y * g_backbuffer_pitch + x];
}

void clear_screen(uint32_t color)
{
    if (!g_framebuffer || !g_backbuffer)
        return;

    uint32_t w = g_framebuffer->framebuffer_width;
    uint32_t h = g_framebuffer->framebuffer_height;

    for (uint32_t y = 0; y < h; y++)
    {
        uint32_t *row = &g_backbuffer[y * g_backbuffer_pitch];
        for (uint32_t x = 0; x < w; x++)
        {
            row[x] = color;
        }
    }
}
void put_pixel_fb(uint32_t x, uint32_t y, uint32_t color)
{
    uint32_t *fb = (uint32_t *)g_framebuffer->framebuffer_addr;
    uint32_t pitch = g_framebuffer->framebuffer_pitch >> 2;

    if (x < g_framebuffer->framebuffer_width &&
        y < g_framebuffer->framebuffer_height)
        fb[y * pitch + x] = color;
}

void blit_backbuffer_region_to_fb(uint32_t x, uint32_t y,
                                  uint32_t w, uint32_t h)
{
    if (!g_framebuffer || !g_backbuffer)
        return;

    uint32_t *fb = (uint32_t *)g_framebuffer->framebuffer_addr;
    uint32_t fb_pitch = g_framebuffer->framebuffer_pitch >> 2;

    uint32_t sw = g_framebuffer->framebuffer_width;
    uint32_t sh = g_framebuffer->framebuffer_height;

    if (x >= sw || y >= sh)
        return;
    if (x + w > sw)
        w = sw - x;
    if (y + h > sh)
        h = sh - y;

    for (uint32_t yy = 0; yy < h; yy++)
    {
        uint32_t src_y = y + yy;
        uint32_t *src = &g_backbuffer[src_y * g_backbuffer_pitch + x];
        uint32_t *dst = &fb[src_y * fb_pitch + x];
        memcpy(dst, src, w * sizeof(uint32_t));
    }
}

/* ===========================
 * 线段（Bresenham）
 * =========================== */

static inline int iabs(int x)
{
    return x < 0 ? -x : x;
}

void draw_line(int x0, int y0, int x1, int y1, uint32_t color)
{
    int dx = iabs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -iabs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    while (1)
    {
        put_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1)
            break;

        int e2 = 2 * err;
        if (e2 >= dy)
        {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx)
        {
            err += dx;
            y0 += sy;
        }
    }
}

/* ===========================
 * 矩形
 * =========================== */

void draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
    draw_line(x, y, x + w, y, color);
    draw_line(x, y, x, y + h, color);
    draw_line(x + w, y, x + w, y + h, color);
    draw_line(x, y + h, x + w, y + h, color);
}

void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
    for (uint32_t yy = y; yy < y + h; yy++)
        for (uint32_t xx = x; xx < x + w; xx++)
            put_pixel(xx, yy, color);
}

void draw_rect_fb(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
    for (uint32_t i = 0; i < w; i++)
    {
        put_pixel_fb(x + i, y, color);
        put_pixel_fb(x + i, y + h - 1, color);
    }
    for (uint32_t i = 0; i < h; i++)
    {
        put_pixel_fb(x, y + i, color);
        put_pixel_fb(x + w - 1, y + i, color);
    }
}

void xor_rect_fb(uint32_t x, uint32_t y,
                 uint32_t w, uint32_t h,
                 uint32_t color)
{
    if (!g_framebuffer)
        return;

    uint32_t *fb = (uint32_t *)g_framebuffer->framebuffer_addr;
    uint32_t pitch = g_framebuffer->framebuffer_pitch >> 2;

    uint32_t sw = g_framebuffer->framebuffer_width;
    uint32_t sh = g_framebuffer->framebuffer_height;

    if (x >= sw || y >= sh)
        return;
    if (x + w > sw)
        w = sw - x;
    if (y + h > sh)
        h = sh - y;
    if (w == 0 || h == 0)
        return;

    for (uint32_t i = 0; i < w; i++)
    {
        uint32_t xx = x + i;
        uint32_t yt = y;
        uint32_t yb = y + h - 1;
        fb[yt * pitch + xx] ^= color;
        if (yb != yt)
            fb[yb * pitch + xx] ^= color;
    }

    for (uint32_t j = 0; j < h; j++)
    {
        uint32_t yy = y + j;
        uint32_t xl = x;
        uint32_t xr = x + w - 1;
        fb[yy * pitch + xl] ^= color;
        if (xr != xl)
            fb[yy * pitch + xr] ^= color;
    }
}

/* ===========================
 * 圆形（中点圆算法）
 * =========================== */

void draw_circle(int xc, int yc, int r, uint32_t color)
{
    int x = 0, y = r;
    int d = 3 - 2 * r;

    while (y >= x)
    {
        put_pixel(xc + x, yc + y, color);
        put_pixel(xc - x, yc + y, color);
        put_pixel(xc + x, yc - y, color);
        put_pixel(xc - x, yc - y, color);
        put_pixel(xc + y, yc + x, color);
        put_pixel(xc - y, yc + x, color);
        put_pixel(xc + y, yc - x, color);
        put_pixel(xc - y, yc - x, color);

        x++;
        if (d > 0)
        {
            y--;
            d = d + 4 * (x - y) + 10;
        }
        else
        {
            d = d + 4 * x + 6;
        }
    }
}

void fill_circle(int xc, int yc, int r, uint32_t color)
{
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++)
            if (x * x + y * y <= r * r)
                put_pixel(xc + x, yc + y, color);
}

void graphics_draw_alpha_bitmap(int x, int y,
                                uint8_t *alpha,
                                int w, int h,
                                uint32_t color)
{
    for (int j = 0; j < h; j++)
    {
        for (int i = 0; i < w; i++)
        {
            uint8_t a = alpha[j * w + i];
            if (a == 0)
                continue; // 0 = 不画

            put_pixel(x + i, y + j, color);
        }
    }
}

/* ===========================
 * 调试输出
 * =========================== */

void print_fb_info()
{
    if (!g_framebuffer)
        return;

    serial_puts("--- Framebuffer Info ---\n");
    serial_puts("Addr: 0x");
    serial_puthex64(g_framebuffer->framebuffer_addr);
    serial_puts("\nRes: ");
    serial_putdec64(g_framebuffer->framebuffer_width);
    serial_puts("x");
    serial_putdec64(g_framebuffer->framebuffer_height);
    serial_puts("\nPitch: ");
    serial_putdec64(g_framebuffer->framebuffer_pitch);
    serial_puts("\nBPP: ");
    serial_putdec64(g_framebuffer->framebuffer_bpp);
    serial_puts("\n------------------------\n");
}
