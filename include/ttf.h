#ifndef TTF_H
#define TTF_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    uint8_t* data;
    size_t   size;

    uint32_t offset_cmap, length_cmap;
    uint32_t offset_head, length_head;
    uint32_t offset_hhea, length_hhea;
    uint32_t offset_hmtx, length_hmtx;
    uint32_t offset_maxp, length_maxp;
    uint32_t offset_glyf, length_glyf;

    // loca 表 + indexToLocFormat
    uint32_t offset_loca, length_loca;
    int16_t  indexToLocFormat;   // 0 = short, 1 = long

    uint16_t numGlyphs;
    uint16_t unitsPerEm;
    int16_t  ascender;
    int16_t  descender;
    int16_t  lineGap;

    // cmap format 4 子表偏移（绝对偏移）
    uint32_t cmap_format4_offset;
} TTF_Font;

typedef struct {
    int16_t x;
    int16_t y;
    uint8_t on_curve;
} TTF_Point;

typedef struct {
    int16_t xMin, yMin, xMax, yMax;
    uint16_t contourCount;
    uint16_t pointCount;
    TTF_Point* points;
    uint16_t* endPts;
} TTF_GlyphOutline;

TTF_Font* ttf_load_from_path(const char* path);
void      ttf_unload(TTF_Font* font);

uint16_t  ttf_get_units_per_em(const TTF_Font* font);
int16_t   ttf_get_ascender(const TTF_Font* font);
int16_t   ttf_get_descender(const TTF_Font* font);
int16_t   ttf_get_line_gap(const TTF_Font* font);

// 字符 / glyph
uint16_t  ttf_char_to_glyph(const TTF_Font* font, uint32_t codepoint);
uint16_t  ttf_get_glyph_advance(const TTF_Font* font, uint16_t glyph_index);

// 渲染单个 glyph（x,y 为基线坐标；pixel_size 为字体像素高度）
void ttf_draw_glyph(const TTF_Font* font,
                    uint16_t glyph_index,
                    int x, int y,
                    int pixel_size,
                    uint32_t color);

void ttf_draw_glyph_fb(const TTF_Font* font,
                    uint16_t glyph_index,
                    int x, int y,
                    int pixel_size,
                    uint32_t color);

// 渲染一行 UTF‑8 文本（支持中文，简单横排）
void ttf_draw_text_utf8(const TTF_Font* font,
                        int x, int y,           // 基线起点
                        int pixel_size,
                        uint32_t color,
                        const char* utf8);

void ttf_draw_text_utf8_fb(const TTF_Font* font,
                        int x, int y,           // 基线起点
                        int pixel_size,
                        uint32_t color,
                        const char* utf8);

// 渲染 UTF-8 文本到指定像素缓冲区（用于窗口内容绘制）
void ttf_draw_text_utf8_buf(const TTF_Font* font,
                        int x, int y,           // 基线起点
                        int pixel_size,
                        uint32_t color,
                        uint32_t* buf,          // 目标缓冲区
                        int buf_w, int buf_h,   // 缓冲区宽高
                        const char* utf8);

// ========================
// Glyph Cache（按 glyph_index + pixel_size）
// ========================

// 对外：清空 cache（可选，在字体卸载或切换时用）
void ttf_glyph_cache_clear(void);

void ttf_play_loading_animation(const char* font_path,
                                int x, int y,
                                uint32_t duration_ms,
                                uint16_t pixel_size);

#endif
