#include "ttf.h"
#include "drivers/fs/fat32.h"
#include "graphics.h"
#include "klog.h"
#include "memory.h"
#include "shell.h"
#include "string.h"
#include "timer.h"
#include <stdint.h>

// 声明 resolve_path 函数（在 shell.c 中定义）
void resolve_path(const char* path, char* full_path);

/* =================================================================
 * stb_truetype 数学 / 内存适配（必须在 #include 之前定义）
 * ================================================================= */
#include <float.h> // 避免 stb 内部需要 math.h

// ifloor / iceil —— 用于 glyph bbox 计算，需正确向负无穷取整
static inline int _stb_ifloor(float x) {
    int i = (int)x;
    if (x < 0.0f && (float)i != x)
        return i - 1;
    return i;
}
static inline int _stb_iceil(float x) {
    int i = (int)x;
    if (x > 0.0f && (float)i != x)
        return i + 1;
    return i;
}

// x87 内联平方根（不使用 libm）
static inline float _stb_sqrtf(float x) {
    float r;
    __asm__ volatile("fsqrt" : "=t"(r) : "0"(x));
    return r;
}

static inline float _stb_fabsf(float x) { return x < 0.0f ? -x : x; }

// SDF 路径用到的函数 —— 正常渲染不会调用，提供桩避免链接错误
static inline float _stb_powf(float x, float y) {
    (void)x;
    (void)y;
    return 0.0f;
}
static inline float _stb_fmodf(float x, float y) {
    (void)x;
    (void)y;
    return 0.0f;
}

#define STBTT_ifloor(x) _stb_ifloor(x)
#define STBTT_iceil(x) _stb_iceil(x)
#define STBTT_sqrt(x) _stb_sqrtf(x)
#define STBTT_fabs(x) _stb_fabsf(x)
#define STBTT_pow(x, y) _stb_powf(x, y)
#define STBTT_fmod(x, y) _stb_fmodf(x, y)
#define STBTT_cos(x) 0.0f
#define STBTT_acos(x) 1.5707963f

// 内存分配 —— 使用内核分配器
#define STBTT_malloc(x, u) kmalloc(x)
#define STBTT_free(x, u) kfree(x)

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

/* =================================================================
 * Glyph Cache（简单哈希）
 * ================================================================= */

typedef struct {
    uint16_t glyph_index;
    uint16_t pixel_size;

    int16_t xMin, yMin; // stb bitmap box：bearing (xMin)，yMin 可为负（上伸）
    int width, height;

    int bearingX; // 基线到 bitmap 左边缘
    int bearingY; // 基线到 bitmap 上边缘（向上为正）

    uint8_t* alpha; // width × height，0 = 空，255 = 实心
    bool used;
} TTF_GlyphCacheEntry;

#define TTF_GLYPH_CACHE_SIZE 1024
static TTF_GlyphCacheEntry g_glyph_cache[TTF_GLYPH_CACHE_SIZE];

static inline uint32_t glyph_hash(uint16_t glyph_index, uint16_t pixel_size) {
    uint32_t h = glyph_index;
    h = (h * 16777619u) ^ pixel_size;
    return h & (TTF_GLYPH_CACHE_SIZE - 1);
}

void ttf_glyph_cache_clear(void) {
    for (uint32_t i = 0; i < TTF_GLYPH_CACHE_SIZE; i++) {
        if (g_glyph_cache[i].used && g_glyph_cache[i].alpha)
            kfree(g_glyph_cache[i].alpha);
        memset(&g_glyph_cache[i], 0, sizeof(TTF_GlyphCacheEntry));
    }
}

static TTF_GlyphCacheEntry* glyph_cache_lookup(uint16_t glyph_index,
                                               uint16_t pixel_size) {
    uint32_t h = glyph_hash(glyph_index, pixel_size);
    for (uint32_t probe = 0; probe < TTF_GLYPH_CACHE_SIZE; probe++) {
        uint32_t idx = (h + probe) & (TTF_GLYPH_CACHE_SIZE - 1);
        TTF_GlyphCacheEntry* e = &g_glyph_cache[idx];
        if (!e->used)
            return NULL;
        if (e->glyph_index == glyph_index && e->pixel_size == pixel_size)
            return e;
    }
    return NULL;
}

static TTF_GlyphCacheEntry* glyph_cache_insert(uint16_t glyph_index,
                                               uint16_t pixel_size) {
    uint32_t h = glyph_hash(glyph_index, pixel_size);
    for (uint32_t probe = 0; probe < TTF_GLYPH_CACHE_SIZE; probe++) {
        uint32_t idx = (h + probe) & (TTF_GLYPH_CACHE_SIZE - 1);
        TTF_GlyphCacheEntry* e = &g_glyph_cache[idx];
        if (!e->used) {
            memset(e, 0, sizeof(*e));
            e->used = true;
            e->glyph_index = glyph_index;
            e->pixel_size = pixel_size;
            return e;
        }
    }
    // 满了就覆盖 hash 位置
    uint32_t idx = h;
    TTF_GlyphCacheEntry* e = &g_glyph_cache[idx & (TTF_GLYPH_CACHE_SIZE - 1)];
    if (e->used && e->alpha)
        kfree(e->alpha);
    memset(e, 0, sizeof(*e));
    e->used = true;
    e->glyph_index = glyph_index;
    e->pixel_size = pixel_size;
    return e;
}

/* =================================================================
 * 用 stb_truetype 渲染 glyph → 填充 cache entry
 * ================================================================= */
static bool render_glyph_to_cache(TTF_GlyphCacheEntry* e, stbtt_fontinfo* stb,
                                  uint16_t glyph_index, uint16_t pixel_size) {
    float scale = stbtt_ScaleForPixelHeight(stb, (float)pixel_size);

    int ix0, iy0, ix1, iy1;
    stbtt_GetGlyphBitmapBox(stb, glyph_index, scale, scale, &ix0, &iy0, &ix1,
                            &iy1);

    int w = ix1 - ix0;
    int h = iy1 - iy0;
    if (w <= 0 || h <= 0)
        return false;

    e->xMin = (int16_t)ix0;
    e->yMin = (int16_t)iy0;
    e->width = w;
    e->height = h;
    e->bearingX = ix0;
    e->bearingY = -iy0; // 向上为正

    e->alpha = (uint8_t*)kmalloc((uint32_t)w * (uint32_t)h);
    if (!e->alpha)
        return false;

    // stb 从字体单位 → 像素，输出 8-bit alpha（0=透明，255=实心）
    stbtt_MakeGlyphBitmap(stb, e->alpha, w, h, w, scale, scale, glyph_index);
    return true;
}

/* =================================================================
 * Alpha 混合
 * ================================================================= */
static uint32_t alpha_blend_over(uint32_t src, uint32_t dst, uint8_t a) {
    if (a == 255)
        return src;
    if (a == 0)
        return dst;

    uint32_t sr = (src >> 16) & 0xFF;
    uint32_t sg = (src >> 8) & 0xFF;
    uint32_t sb = (src >> 0) & 0xFF;
    uint32_t dr = (dst >> 16) & 0xFF;
    uint32_t dg = (dst >> 8) & 0xFF;
    uint32_t db = (dst >> 0) & 0xFF;

    uint32_t r = (sr * a + dr * (255 - a)) / 255;
    uint32_t g = (sg * a + dg * (255 - a)) / 255;
    uint32_t b = (sb * a + db * (255 - a)) / 255;
    return (0xFFu << 24) | (r << 16) | (g << 8) | b;
}

/* =================================================================
 * Blit 函数
 *
 * stb 约定：bitmap 行序为屏幕从上到下（Y 向下）。
 * yMin = iy0（可为负，表示基线以上的部分），
 * dst_y0 = y + yMin（屏幕坐标），
 * sy = dst_y0 + by（by=0 是最顶行）。
 * ================================================================= */

static void blit_glyph_bitmap(const TTF_GlyphCacheEntry* e, int x, int y,
                              uint32_t color) {
    if (!e || !e->alpha || !g_backbuffer)
        return;

    int dst_x0 = x + e->xMin;
    int dst_y0 = y + e->yMin;
    int scr_w = g_framebuffer->framebuffer_width;
    int scr_h = g_framebuffer->framebuffer_height;

    // 预裁剪 Y
    int by_min = 0, by_max = e->height - 1;
    if (dst_y0 + by_min < 0)
        by_min = -dst_y0;
    if (dst_y0 + by_max >= scr_h)
        by_max = scr_h - 1 - dst_y0;
    if (by_min > by_max)
        return;

    // 预裁剪 X
    int bx_min = 0, bx_max = e->width - 1;
    if (dst_x0 + bx_min < 0)
        bx_min = -dst_x0;
    if (dst_x0 + bx_max >= scr_w)
        bx_max = scr_w - 1 - dst_x0;
    if (bx_min > bx_max)
        return;

    for (int by = by_min; by <= by_max; by++) {
        int sy = dst_y0 + by;
        const uint8_t* row = &e->alpha[by * e->width];
        uint32_t* screen_row = &g_backbuffer[sy * g_backbuffer_pitch];
        for (int bx = bx_min; bx <= bx_max; bx++) {
            uint8_t a = row[bx];
            if (!a)
                continue;
            int sx = dst_x0 + bx;
            screen_row[sx] = alpha_blend_over(color, screen_row[sx], a);
        }
    }
}

static void blit_glyph_bitmap_buf(const TTF_GlyphCacheEntry* e, int x, int y,
                                  uint32_t color, uint32_t* buf, int buf_w,
                                  int buf_h) {
    if (!e || !e->alpha || !buf)
        return;

    int dst_x0 = x + e->xMin;
    int dst_y0 = y + e->yMin;

    int by_min = 0, by_max = e->height - 1;
    if (dst_y0 + by_min < 0)
        by_min = -dst_y0;
    if (dst_y0 + by_max >= buf_h)
        by_max = buf_h - 1 - dst_y0;
    if (by_min > by_max)
        return;

    int bx_min = 0, bx_max = e->width - 1;
    if (dst_x0 + bx_min < 0)
        bx_min = -dst_x0;
    if (dst_x0 + bx_max >= buf_w)
        bx_max = buf_w - 1 - dst_x0;
    if (bx_min > bx_max)
        return;

    for (int by = by_min; by <= by_max; by++) {
        int sy = dst_y0 + by;
        const uint8_t* row = &e->alpha[by * e->width];
        uint32_t* buf_row = &buf[sy * buf_w];
        for (int bx = bx_min; bx <= bx_max; bx++) {
            uint8_t a = row[bx];
            if (!a)
                continue;
            int sx = dst_x0 + bx;
            buf_row[sx] = alpha_blend_over(color, buf_row[sx], a);
        }
    }
}

static void blit_glyph_bitmap_fb(const TTF_GlyphCacheEntry* e, int x, int y,
                                 uint32_t color) {
    blit_glyph_bitmap(e, x, y, color);
}

/* =================================================================
 * 对外渲染接口：单 glyph
 * ================================================================= */

void ttf_draw_glyph(const TTF_Font* font, uint16_t glyph_index, int x, int y,
                    int pixel_size, uint32_t color) {
    if (!font || pixel_size <= 0 || !font->stb_font)
        return;

    uint16_t ps = (uint16_t)pixel_size;
    TTF_GlyphCacheEntry* e = glyph_cache_lookup(glyph_index, ps);
    if (!e) {
        e = glyph_cache_insert(glyph_index, ps);
        if (!render_glyph_to_cache(e, (stbtt_fontinfo*)font->stb_font,
                                   glyph_index, ps)) {
            e->used = false;
            return;
        }
    }
    blit_glyph_bitmap(e, x, y, color);
}

void ttf_draw_glyph_fb(const TTF_Font* font, uint16_t glyph_index, int x, int y,
                       int pixel_size, uint32_t color) {
    if (!font || pixel_size <= 0 || !font->stb_font)
        return;

    uint16_t ps = (uint16_t)pixel_size;
    TTF_GlyphCacheEntry* e = glyph_cache_lookup(glyph_index, ps);
    if (!e) {
        e = glyph_cache_insert(glyph_index, ps);
        if (!render_glyph_to_cache(e, (stbtt_fontinfo*)font->stb_font,
                                   glyph_index, ps)) {
            e->used = false;
            return;
        }
    }
    blit_glyph_bitmap_fb(e, x, y, color);
}

void ttf_draw_glyph_buf(const TTF_Font* font, uint16_t glyph_index, int x,
                        int y, int pixel_size, uint32_t color, uint32_t* buf,
                        int buf_w, int buf_h) {
    if (!font || pixel_size <= 0 || !buf || !font->stb_font)
        return;

    uint16_t ps = (uint16_t)pixel_size;
    TTF_GlyphCacheEntry* e = glyph_cache_lookup(glyph_index, ps);
    if (!e) {
        e = glyph_cache_insert(glyph_index, ps);
        if (!render_glyph_to_cache(e, (stbtt_fontinfo*)font->stb_font,
                                   glyph_index, ps)) {
            e->used = false;
            return;
        }
    }
    blit_glyph_bitmap_buf(e, x, y, color, buf, buf_w, buf_h);
}

/* =================================================================
 * 字体加载 / 卸载
 * ================================================================= */

TTF_Font* ttf_load_from_path(const char* path) {
    char full[256];
    resolve_path(path, full);
    kinfo("TTF: 加载字体: %s", full);

    fat32_handle_t fh;
    if (!fat32_open(full, &fh, FILE_READ)) {
        kerror("TTF: 无法打开字体文件: %s", full);
        return NULL;
    }

    size_t size = fh.file_size;
    uint8_t* buf = (uint8_t*)kmalloc(size);
    if (!buf) {
        kerror("TTF: 内存不足");
        fat32_close(&fh);
        return NULL;
    }

    if (!fat32_read(&fh, buf, size)) {
        kerror("TTF: 读取失败: %s", full);
        kfree(buf);
        fat32_close(&fh);
        return NULL;
    }
    fat32_close(&fh);

    TTF_Font* font = (TTF_Font*)kmalloc(sizeof(TTF_Font));
    if (!font) {
        kerror("TTF: 内存不足（TTF_Font）");
        kfree(buf);
        return NULL;
    }
    memset(font, 0, sizeof(TTF_Font));
    font->data = buf;
    font->size = size;

    // 分配 stb_truetype 内部状态
    stbtt_fontinfo* stb = (stbtt_fontinfo*)kmalloc(sizeof(stbtt_fontinfo));
    if (!stb) {
        kerror("TTF: 内存不足（stbtt_fontinfo）");
        kfree(buf);
        kfree(font);
        return NULL;
    }

    int init_ok = stbtt_InitFont(stb, buf, 0);

    if (!init_ok) {
        kerror("TTF: stbtt_InitFont 失败");
        kfree(stb);
        kfree(buf);
        kfree(font);
        return NULL;
    }
    font->stb_font = stb;

    // 缓存度量
    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(stb, &ascent, &descent, &lineGap);

    // stb 的 head 字段存储的是 head 表的文件偏移（int），不是指针
    // unitsPerEm 在 head 表偏移 + 18 处，UInt16 大端
    if (stb->head > 0)
        font->unitsPerEm =
            (uint16_t)((buf[stb->head + 18] << 8) | buf[stb->head + 19]);
    else
        font->unitsPerEm = 1000; // 典型默认值

    font->ascender = (int16_t)ascent;
    font->descender = (int16_t)descent;
    font->lineGap = (int16_t)lineGap;

    kinfo("TTF: 加载成功: upm=%u, asc=%d, desc=%d", font->unitsPerEm,
          font->ascender, font->descender);

    return font;
}

void ttf_unload(TTF_Font* font) {
    if (!font)
        return;
    if (font->stb_font)
        kfree(font->stb_font);
    if (font->data)
        kfree(font->data);
    kfree(font);
}

/* =================================================================
 * 基本度量 & glyph 映射
 * ================================================================= */

uint16_t ttf_get_units_per_em(const TTF_Font* font) {
    return font ? font->unitsPerEm : 0;
}
int16_t ttf_get_ascender(const TTF_Font* font) {
    return font ? font->ascender : 0;
}
int16_t ttf_get_descender(const TTF_Font* font) {
    return font ? font->descender : 0;
}
int16_t ttf_get_line_gap(const TTF_Font* font) {
    return font ? font->lineGap : 0;
}

uint16_t ttf_char_to_glyph(const TTF_Font* font, uint32_t codepoint) {
    if (!font || !font->stb_font)
        return 0;
    return (uint16_t)stbtt_FindGlyphIndex((stbtt_fontinfo*)font->stb_font,
                                          (int)codepoint);
}

uint16_t ttf_get_glyph_advance(const TTF_Font* font, uint16_t glyph_index) {
    if (!font || !font->stb_font)
        return 0;
    int advance;
    stbtt_GetGlyphHMetrics((stbtt_fontinfo*)font->stb_font, (int)glyph_index,
                           &advance, NULL);
    return (uint16_t)advance;
}

/* =================================================================
 * UTF-8 解码 + 文本渲染
 * ================================================================= */

static uint32_t utf8_decode(const char** p) {
    const uint8_t* s = (const uint8_t*)*p;
    uint32_t cp;

    if (s[0] < 0x80) {
        cp = s[0];
        *p += 1;
    } else if ((s[0] & 0xE0) == 0xC0) {
        cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);
        *p += 2;
    } else if ((s[0] & 0xF0) == 0xE0) {
        cp = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        *p += 3;
    } else if ((s[0] & 0xF8) == 0xF0) {
        cp = ((s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12) |
             ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        *p += 4;
    } else {
        cp = 0xFFFD;
        *p += 1;
    }
    return cp;
}

static int get_unit_advance(const TTF_Font* font, uint16_t glyph) {
    if (!font || !font->stb_font)
        return 0;
    int adv;
    stbtt_GetGlyphHMetrics((stbtt_fontinfo*)font->stb_font, glyph, &adv, NULL);
    return adv;
}

void ttf_draw_text_utf8(const TTF_Font* font, int x, int y, int pixel_size,
                        uint32_t color, const char* utf8) {
    if (!font || !utf8)
        return;

    int pen_x = x;
    const char* p = utf8;

    while (*p) {
        uint32_t cp = utf8_decode(&p);
        if (cp == '\n') {
            int lh = (int)((int64_t)(font->ascender - font->descender +
                                     font->lineGap) *
                           pixel_size / font->unitsPerEm);
            pen_x = x;
            y += lh;
            continue;
        }

        uint16_t glyph = ttf_char_to_glyph(font, cp);
        if (glyph == 0) {
            int adv = get_unit_advance(font, ttf_char_to_glyph(font, ' '));
            pen_x += (int)((int64_t)adv * pixel_size / font->unitsPerEm);
            continue;
        }

        ttf_draw_glyph(font, glyph, pen_x, y, pixel_size, color);
        int adv = get_unit_advance(font, glyph);
        pen_x += (int)((int64_t)adv * pixel_size / font->unitsPerEm);
    }
}

void ttf_draw_text_utf8_fb(const TTF_Font* font, int x, int y, int pixel_size,
                           uint32_t color, const char* utf8) {
    if (!font || !utf8)
        return;

    int pen_x = x;
    const char* p = utf8;

    while (*p) {
        uint32_t cp = utf8_decode(&p);
        if (cp == '\n') {
            int lh = (int)((int64_t)(font->ascender - font->descender +
                                     font->lineGap) *
                           pixel_size / font->unitsPerEm);
            pen_x = x;
            y += lh;
            continue;
        }

        uint16_t glyph = ttf_char_to_glyph(font, cp);
        if (glyph == 0) {
            int adv = get_unit_advance(font, ttf_char_to_glyph(font, ' '));
            pen_x += (int)((int64_t)adv * pixel_size / font->unitsPerEm);
            continue;
        }

        ttf_draw_glyph_fb(font, glyph, pen_x, y, pixel_size, color);
        int adv = get_unit_advance(font, glyph);
        pen_x += (int)((int64_t)adv * pixel_size / font->unitsPerEm);
    }
}

void ttf_draw_text_utf8_buf(const TTF_Font* font, int x, int y, int pixel_size,
                            uint32_t color, uint32_t* buf, int buf_w, int buf_h,
                            const char* utf8) {
    if (!font || !utf8 || !buf)
        return;

    int pen_x = x;
    const char* p = utf8;

    while (*p) {
        uint32_t cp = utf8_decode(&p);
        if (cp == '\n') {
            int lh = (int)((int64_t)(font->ascender - font->descender +
                                     font->lineGap) *
                           pixel_size / font->unitsPerEm);
            pen_x = x;
            y += lh;
            continue;
        }

        uint16_t glyph = ttf_char_to_glyph(font, cp);
        if (glyph == 0) {
            int adv = get_unit_advance(font, ttf_char_to_glyph(font, ' '));
            pen_x += (int)((int64_t)adv * pixel_size / font->unitsPerEm);
            continue;
        }

        ttf_draw_glyph_buf(font, glyph, pen_x, y, pixel_size, color, buf, buf_w,
                           buf_h);
        int adv = get_unit_advance(font, glyph);
        pen_x += (int)((int64_t)adv * pixel_size / font->unitsPerEm);
    }
}

/* =================================================================
 * ttf_render_glyph / ttf_render_glyph_fb —— 供外部直接使用
 * ================================================================= */

TTF_GlyphCacheEntry* ttf_render_glyph(TTF_Font* font, uint16_t glyph_index,
                                      uint16_t pixel_size) {
    if (!font || !font->stb_font)
        return NULL;

    TTF_GlyphCacheEntry* e = glyph_cache_lookup(glyph_index, pixel_size);
    if (e)
        return e;

    e = glyph_cache_insert(glyph_index, pixel_size);
    if (!render_glyph_to_cache(e, (stbtt_fontinfo*)font->stb_font, glyph_index,
                               pixel_size)) {
        e->used = false;
        return NULL;
    }
    return e;
}

TTF_GlyphCacheEntry* ttf_render_glyph_fb(TTF_Font* font, uint16_t glyph_index,
                                         uint16_t pixel_size) {
    return ttf_render_glyph(font, glyph_index, pixel_size);
}

/* =================================================================
 * ttf_dump_codepoints —— 扫描 PUA 字符
 * ================================================================= */

void ttf_dump_codepoints(TTF_Font* font) {
    if (!font || !font->stb_font)
        return;
    stbtt_fontinfo* stb = (stbtt_fontinfo*)font->stb_font;

    for (uint32_t cp = 0; cp <= 0xFFFF; cp++) {
        int g = stbtt_FindGlyphIndex(stb, (int)cp);
        if (g != 0) {
            if (cp >= 0xE000 && cp <= 0xF8FF)
                shell_printf("PUA U+%04X -> glyph %d\n", cp, g);
        }
    }
}

/* =================================================================
 * 加载动画
 * ================================================================= */

void ttf_play_loading_animation(const char* font_path, int x, int y,
                                uint32_t duration_ms, uint16_t pixel_size) {
    serial_puts("ttf 1\n");
    TTF_Font* font = ttf_load_from_path(font_path);
    if (!font)
        return;
    ttf_dump_codepoints(font);
    uint16_t frames[64];
    int frame_count = 0;

    for (uint32_t cp = 0xE052; cp <= 0xE0CB; cp++) {
        uint16_t g = ttf_char_to_glyph(font, cp);
        if (g != 0)
            frames[frame_count++] = (uint16_t)cp;
    }
    serial_puts("ANIM: frame_count = ");
    serial_putdec64(frame_count);
    serial_puts("\n");
    if (frame_count == 0) {
        ttf_unload(font);
        return;
    }
    serial_puts("ttf 2\n");
    uint64_t start = timer_ms();
    int frame = 0;

    while (timer_ms() - start < duration_ms) {
        serial_puts("ttf w\n");
        uint16_t cp = frames[frame];
        uint16_t glyph = ttf_char_to_glyph(font, cp);

        TTF_GlyphCacheEntry* e = ttf_render_glyph(font, glyph, pixel_size);
        if (e) {
            graphics_draw_alpha_bitmap(x + e->bearingX, y - e->bearingY,
                                       e->alpha, e->width, e->height,
                                       0xFFFFFFFF);
        }

        frame = (frame + 1) % frame_count;
        sleep_ms(100);
        graphics_present();
    }

    ttf_unload(font);
}