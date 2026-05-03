#ifndef GRAPHICS_H
#define GRAPHICS_H

#include "kernel.h"
#include <stdint.h>

// ARGB 颜色宏
#define ARGB(a,r,g,b)   ((uint32_t)((a)<<24 | (r)<<16 | (g)<<8 | (b)))
#define RGB(r,g,b)      ARGB(0xFF, r, g, b)

#define COLOR_BLACK     RGB(0,0,0)
#define COLOR_WHITE     RGB(255,255,255)
#define COLOR_RED       RGB(255,0,0)
#define COLOR_GREEN     RGB(0,255,0)
#define COLOR_BLUE      RGB(0,0,255)
#define COLOR_CYAN      RGB(0,255,255)
#define COLOR_MAGENTA   RGB(255,0,255)
#define COLOR_YELLOW    RGB(255,255,0)
#define COLOR_GRAY      RGB(128,128,128)

extern boot_params_t *g_framebuffer;
extern uint32_t* g_backbuffer;
extern uint32_t g_backbuffer_pitch;

void graphics_init(boot_params_t *fb_info);
void graphics_present(void);
void put_pixel(uint32_t x, uint32_t y, uint32_t color);
void clear_screen(uint32_t color);
uint32_t get_pixel(int x, int y);
void put_pixel_fb(uint32_t x, uint32_t y, uint32_t color);
void draw_line(int x0, int y0, int x1, int y1, uint32_t color);
void draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void draw_circle(int xc, int yc, int r, uint32_t color);
void fill_circle(int xc, int yc, int r, uint32_t color);
void draw_rect_fb(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void blit_backbuffer_region_to_fb(uint32_t x, uint32_t y,uint32_t w, uint32_t h);
void xor_rect_fb(uint32_t x, uint32_t y,
                 uint32_t w, uint32_t h,
                 uint32_t color);
                                  
void draw_char(int x, int y, char c, uint32_t color);
void draw_text(int x, int y, const char* text, uint32_t color);

void graphics_draw_alpha_bitmap(int x, int y,
                                uint8_t* alpha,
                                int w, int h,
                                uint32_t color);
                                
void print_fb_info();

#endif
