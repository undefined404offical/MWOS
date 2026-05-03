#include "ttf.h"
#include "drivers/fs/fat32.h"
#include "graphics.h"
#include "klog.h"
#include "memory.h"
#include "shell.h"
#include "string.h"
#include "ttf_cache.h"
#include <stdint.h>

// 声明resolve_path函数（在shell.c中定义）
void resolve_path(const char *path, char *full_path);

#define TTF_SS_FACTOR 4 // 4x 超采样

/* ========================
 * Glyph Cache（简单哈希）
 * ======================== */

// #define TTF_GLYPH_CACHE_SIZE 2048 // 2K entries (已移至ttf_cache.h)
typedef struct {
    uint16_t glyph_index;
    uint16_t pixel_size;

    int16_t xMin, yMin, xMax, yMax; // 像素空间 bbox
    int width;
    int height;

    int bearingX; // 基线 (x,y) 到 bitmap 左上的偏移
    int bearingY; // 基线 (x,y) 到 bitmap 左上的偏移（向上为正）

    uint8_t* alpha; // width * height，0=空，255=实心（暂时二值，后面扩展AA）
    bool used;
} TTF_GlyphCacheEntry;

static TTF_GlyphCacheEntry g_glyph_cache[TTF_GLYPH_CACHE_SIZE];

static inline uint32_t glyph_hash(uint16_t glyph_index, uint16_t pixel_size) {
    uint32_t h = glyph_index;
    h = (h * 16777619u) ^ pixel_size;
    return h & (TTF_GLYPH_CACHE_SIZE - 1); // 要保证表大小是 2 的幂
}

void ttf_glyph_cache_clear(void) {
    for (uint32_t i = 0; i < TTF_GLYPH_CACHE_SIZE; i++) {
        if (g_glyph_cache[i].used && g_glyph_cache[i].alpha) {
            kfree(g_glyph_cache[i].alpha);
        }
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
        if (e->glyph_index == glyph_index && e->pixel_size == pixel_size) {
            return e;
        }
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
    // 简单策略：满了就覆盖 hash 位置
    uint32_t idx = h;
    TTF_GlyphCacheEntry* e = &g_glyph_cache[idx & (TTF_GLYPH_CACHE_SIZE - 1)];
    if (e->used && e->alpha) {
        kfree(e->alpha);
    }
    memset(e, 0, sizeof(*e));
    e->used = true;
    e->glyph_index = glyph_index;
    e->pixel_size = pixel_size;
    return e;
}

/* ========================
 * 工具：大端读取
 * ======================== */

static uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

static uint32_t be32(const uint8_t* p) {
    return (uint32_t)(p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]);
}

static void ttf_free_outline(TTF_GlyphOutline* g);
/* ========================
 * 定点数工具（16.16）
 * ======================== */

typedef int32_t fixed;

#define FIXED_SHIFT 16
#define FIXED_ONE (1 << FIXED_SHIFT)

static inline fixed int_to_fixed(int x) { return (fixed)(x << FIXED_SHIFT); }

static inline int fixed_to_int(fixed x) { return (int)(x >> FIXED_SHIFT); }

static inline fixed fixed_mul(fixed a, fixed b) {
    return (fixed)(((int64_t)a * (int64_t)b) >> FIXED_SHIFT);
}

static inline fixed fixed_div(fixed a, fixed b) {
    return (fixed)(((int64_t)a << FIXED_SHIFT) / (int64_t)b);
}

/* 用于 scanline 的边 */
typedef struct {
    fixed x0, y0;
    fixed x1, y1;
} Edge;

/* ========================
 * 查表：sfnt 目录
 * ======================== */

static bool ttf_find_table(const uint8_t* data, size_t size, const char tag[4],
                           uint32_t* offset, uint32_t* length) {
    if (size < 12)
        return false;

    uint16_t numTables = be16(data + 4);
    const uint8_t* tableDir = data + 12;

    if (size < 12 + numTables * 16)
        return false;

    for (uint16_t i = 0; i < numTables; i++) {
        const uint8_t* entry = tableDir + i * 16;
        char t[4];
        t[0] = entry[0];
        t[1] = entry[1];
        t[2] = entry[2];
        t[3] = entry[3];

        if (t[0] == tag[0] && t[1] == tag[1] && t[2] == tag[2] &&
            t[3] == tag[3]) {

            uint32_t off = be32(entry + 8);
            uint32_t len = be32(entry + 12);
            if (off + len > size)
                return false;

            *offset = off;
            *length = len;
            return true;
        }
    }
    return false;
}

/* ========================
 * TTF 加载（从 FAT32）
 * ======================== */

TTF_Font* ttf_load_from_path(const char* path) {
    char full[256];
    resolve_path(path, full);
    kinfo("TTF: 加载字体成功: %s", full);

    fat32_handle_t fh;
    if (!fat32_open(full, &fh, FILE_READ)) {
        kerror("TTF: 无法打开字体文件: %s", full);
        return NULL;
    }

    size_t size = fh.file_size;
    uint8_t* buf = (uint8_t*)kmalloc(size);
    if (!buf) {
        kerror("TTF: 内存不足，无法加载: %s", full);
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

    // 必要表：加上 loca
    if (!ttf_find_table(buf, size, "cmap", &font->offset_cmap,
                        &font->length_cmap) ||
        !ttf_find_table(buf, size, "head", &font->offset_head,
                        &font->length_head) ||
        !ttf_find_table(buf, size, "hhea", &font->offset_hhea,
                        &font->length_hhea) ||
        !ttf_find_table(buf, size, "hmtx", &font->offset_hmtx,
                        &font->length_hmtx) ||
        !ttf_find_table(buf, size, "maxp", &font->offset_maxp,
                        &font->length_maxp) ||
        !ttf_find_table(buf, size, "glyf", &font->offset_glyf,
                        &font->length_glyf) ||
        !ttf_find_table(buf, size, "loca", &font->offset_loca,
                        &font->length_loca)) {

        kerror("TTF: 缺少必要表 (cmap/head/hhea/hmtx/maxp/glyf/loca)\n");
        ttf_unload(font);
        return NULL;
    }

    // maxp
    const uint8_t* maxp = buf + font->offset_maxp;
    font->numGlyphs = be16(maxp + 4);

    // head
    const uint8_t* head = buf + font->offset_head;
    font->unitsPerEm = be16(head + 18);
    font->indexToLocFormat = (int16_t)be16(head + 50);

    // hhea
    const uint8_t* hhea = buf + font->offset_hhea;
    font->ascender = (int16_t)be16(hhea + 4);
    font->descender = (int16_t)be16(hhea + 6);
    font->lineGap = (int16_t)be16(hhea + 8);

    // cmap
    const uint8_t* cmap = buf + font->offset_cmap;
    uint16_t numSubtables = be16(cmap + 2);

    const uint8_t* rec = cmap + 4;
    font->cmap_format4_offset = 0;

    for (uint16_t i = 0; i < numSubtables; i++) {
        uint16_t platformID = be16(rec);
        uint16_t encodingID = be16(rec + 2);
        uint32_t subOffset = be32(rec + 4);
        const uint8_t* sub = cmap + subOffset;
        uint16_t format = be16(sub);

        if (platformID == 3 && (encodingID == 1 || encodingID == 0) &&
            format == 4) {
            font->cmap_format4_offset = font->offset_cmap + subOffset;
            break;
        }

        rec += 8;
    }

    if (!font->cmap_format4_offset) {
        kerror("TTF: 未找到 format 4 cmap 子表");
    } else {
        kinfo("TTF: 找到 format 4 cmap 子表");
    }

    kinfo("TTF: 加载成功: %s\n", full);
    kinfo("  glyphs=%u, upm=%u, asc=%d, desc=%d, locFmt=%d\n", font->numGlyphs,
          font->unitsPerEm, font->ascender, font->descender,
          font->indexToLocFormat);

    return font;
}

void ttf_unload(TTF_Font* font) {
    if (!font)
        return;
    if (font->data)
        kfree(font->data);
    kfree(font);
}

/* ========================
 * 基本度量 & cmap
 * ======================== */

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
    if (!font || !font->cmap_format4_offset)
        return 0;
    if (codepoint > 0xFFFF)
        return 0;

    const uint8_t* sub = font->data + font->cmap_format4_offset;
    uint16_t format = be16(sub);
    if (format != 4)
        return 0;

    uint16_t segCountX2 = be16(sub + 6);
    uint16_t segCount = segCountX2 / 2;

    const uint8_t* endCode = sub + 14;
    const uint8_t* startCode = endCode + segCount * 2 + 2;
    const uint8_t* idDelta = startCode + segCount * 2;
    const uint8_t* idRangeOffset = idDelta + segCount * 2;

    uint16_t c = (uint16_t)codepoint;
    int segment = -1;

    for (uint16_t i = 0; i < segCount; i++) {
        uint16_t end = be16(endCode + i * 2);
        uint16_t start = be16(startCode + i * 2);
        if (c >= start && c <= end) {
            segment = i;
            break;
        }
    }

    if (segment == -1)
        return 0;

    uint16_t start = be16(startCode + segment * 2);
    uint16_t delta = be16(idDelta + segment * 2);
    uint16_t roffset = be16(idRangeOffset + segment * 2);

    if (roffset == 0) {
        return (uint16_t)(c + delta);
    } else {
        uint16_t offset = (uint16_t)(roffset / 2 + (c - start));
        const uint8_t* p = idRangeOffset + segment * 2 + offset * 2;
        uint16_t glyphId = be16(p);
        if (glyphId == 0)
            return 0;
        return (uint16_t)(glyphId + delta);
    }
}

uint16_t ttf_get_glyph_advance(const TTF_Font* font, uint16_t glyph_index) {
    if (!font)
        return 0;
    const uint8_t* hhea = font->data + font->offset_hhea;
    uint16_t numberOfHMetrics = be16(hhea + 34);

    const uint8_t* hmtx = font->data + font->offset_hmtx;

    if (glyph_index < numberOfHMetrics) {
        const uint8_t* m = hmtx + glyph_index * 4;
        return be16(m);
    } else {
        const uint8_t* last = hmtx + (numberOfHMetrics - 1) * 4;
        return be16(last);
    }
}

/* ========================
 * glyf 访问 & simple glyph 解析
 * ======================== */

// 使用 loca 表获取 glyph 的偏移
static const uint8_t* ttf_get_glyph_ptr(const TTF_Font* font,
                                        uint16_t glyph_index) {
    if (!font)
        return NULL;

    const uint8_t* base = font->data;
    const uint8_t* loca = base + font->offset_loca;
    const uint8_t* glyf = base + font->offset_glyf;

    if (glyph_index >= font->numGlyphs)
        return NULL;

    uint32_t offset, next_offset;

    if (font->indexToLocFormat == 0) {
        uint32_t off1 = be16(loca + glyph_index * 2);
        uint32_t off2 = be16(loca + (glyph_index + 1) * 2);
        offset = off1 * 2;
        next_offset = off2 * 2;
    } else {
        offset = be32(loca + glyph_index * 4);
        next_offset = be32(loca + (glyph_index + 1) * 4);
    }

    if (offset == next_offset)
        return NULL; // 空 glyph（比如空格）

    return glyf + offset;
}

static bool ttf_load_simple_glyph(const TTF_Font* font, const uint8_t* gptr,
                                  TTF_GlyphOutline* out) {
    if (!gptr)
        return false;

    int16_t numberOfContours = (int16_t)be16(gptr);
    if (numberOfContours <= 0)
        return false; // composite 先不处理

    out->contourCount = numberOfContours;
    out->xMin = (int16_t)be16(gptr + 2);
    out->yMin = (int16_t)be16(gptr + 4);
    out->xMax = (int16_t)be16(gptr + 6);
    out->yMax = (int16_t)be16(gptr + 8);

    const uint8_t* p = gptr + 10;

    out->endPts = (uint16_t*)kmalloc(sizeof(uint16_t) * numberOfContours);
    if (!out->endPts)
        return false;

    for (int i = 0; i < numberOfContours; i++) {
        out->endPts[i] = be16(p + i * 2);
    }
    p += numberOfContours * 2;

    uint16_t instructionLength = be16(p);
    p += 2 + instructionLength;

    out->pointCount = out->endPts[numberOfContours - 1] + 1;
    out->points = (TTF_Point*)kmalloc(sizeof(TTF_Point) * out->pointCount);
    if (!out->points) {
        kfree(out->endPts);
        return false;
    }

    uint8_t* flags = (uint8_t*)kmalloc(out->pointCount);
    if (!flags) {
        kfree(out->points);
        kfree(out->endPts);
        return false;
    }

    uint16_t i = 0;
    while (i < out->pointCount) {
        uint8_t f = *p++;
        flags[i++] = f;
        if (f & 0x08) {
            uint8_t count = *p++;
            for (uint8_t c = 0; c < count && i < out->pointCount; c++) {
                flags[i++] = f;
            }
        }
    }

    int16_t x = 0;
    for (i = 0; i < out->pointCount; i++) {
        uint8_t f = flags[i];
        int16_t dx = 0;
        if (f & 0x02) {
            uint8_t v = *p++;
            dx = (f & 0x10) ? v : -v;
        } else {
            if (!(f & 0x10)) {
                dx = (int16_t)be16(p);
                p += 2;
            } else {
                dx = 0;
            }
        }
        x += dx;
        out->points[i].x = x;
    }

    int16_t y = 0;
    for (i = 0; i < out->pointCount; i++) {
        uint8_t f = flags[i];
        int16_t dy = 0;
        if (f & 0x04) {
            uint8_t v = *p++;
            dy = (f & 0x20) ? v : -v;
        } else {
            if (!(f & 0x20)) {
                dy = (int16_t)be16(p);
                p += 2;
            } else {
                dy = 0;
            }
        }
        y += dy;
        out->points[i].y = y;
        out->points[i].on_curve = (f & 0x01) ? 1 : 0;
    }

    kfree(flags);
    return true;
}

static bool ttf_load_glyph_outline(const TTF_Font* font, uint16_t glyph_index,
                                   TTF_GlyphOutline* out);

static bool ttf_load_glyph_outline(const TTF_Font* font, uint16_t glyph_index,
                                   TTF_GlyphOutline* out) {
    memset(out, 0, sizeof(*out));

    const uint8_t* gptr = ttf_get_glyph_ptr(font, glyph_index);
    if (!gptr)
        return false;

    int16_t numberOfContours = (int16_t)be16(gptr);
    int16_t xMin = (int16_t)be16(gptr + 2);
    int16_t yMin = (int16_t)be16(gptr + 4);
    int16_t xMax = (int16_t)be16(gptr + 6);
    int16_t yMax = (int16_t)be16(gptr + 8);

    // simple glyph
    if (numberOfContours > 0) {
        // 直接走你原来的 simple 逻辑
        TTF_GlyphOutline tmp;
        memset(&tmp, 0, sizeof(tmp));
        if (!ttf_load_simple_glyph(font, gptr, &tmp)) {
            return false;
        }

        // 把 bbox 和计数复制过去
        *out = tmp;
        out->xMin = xMin;
        out->yMin = yMin;
        out->xMax = xMax;
        out->yMax = yMax;
        return true;
    }

    // 空 glyph（例如空格）
    if (numberOfContours == 0) {
        out->xMin = xMin;
        out->yMin = yMin;
        out->xMax = xMax;
        out->yMax = yMax;
        out->contourCount = 0;
        out->pointCount = 0;
        out->points = NULL;
        out->endPts = NULL;
        return true;
    }

    // composite glyph: numberOfContours == -1
    const uint8_t* p = gptr + 10;

    // 先初始化一个“空”的 outline，后面不断往里合并 component
    out->xMin = xMin;
    out->yMin = yMin;
    out->xMax = xMax;
    out->yMax = yMax;
    out->contourCount = 0;
    out->pointCount = 0;
    out->points = NULL;
    out->endPts = NULL;

    enum {
        ARG_1_AND_2_ARE_WORDS = 0x0001,
        ARGS_ARE_XY_VALUES = 0x0002,
        ROUND_XY_TO_GRID = 0x0004,
        WE_HAVE_A_SCALE = 0x0008,
        MORE_COMPONENTS = 0x0020,
        WE_HAVE_AN_X_AND_Y_SCALE = 0x0040,
        WE_HAVE_A_TWO_BY_TWO = 0x0080,
        WE_HAVE_INSTRUCTIONS = 0x0100,
        USE_MY_METRICS = 0x0200,
        SCALED_COMPONENT_OFFSET = 0x0800,
        UNSCALED_COMPONENT_OFFSET = 0x1000
    };

    bool has_more = true;
    while (has_more) {
        if (p + 4 > font->data + font->size) {
            ttf_free_outline(out);
            return false;
        }

        uint16_t flags = be16(p);
        p += 2;
        uint16_t compIndex = be16(p);
        p += 2;

        int16_t arg1, arg2;
        if (flags & ARG_1_AND_2_ARE_WORDS) {
            if (p + 4 > font->data + font->size) {
                ttf_free_outline(out);
                return false;
            }
            arg1 = (int16_t)be16(p);
            p += 2;
            arg2 = (int16_t)be16(p);
            p += 2;
        } else {
            if (p + 2 > font->data + font->size) {
                ttf_free_outline(out);
                return false;
            }
            int8_t a = (int8_t)p[0];
            int8_t b = (int8_t)p[1];
            p += 2;
            arg1 = a;
            arg2 = b;
        }

        int16_t dx = 0, dy = 0;
        if (flags & ARGS_ARE_XY_VALUES) {
            // arg1, arg2 是直接的 offset
            dx = arg1;
            dy = arg2;
        } else {
            dx = 0;
            dy = 0;
        }

        // 解析变换矩阵（2x2 + 平移），用 16.16 fixed 表示
        fixed m00 = FIXED_ONE;
        fixed m01 = 0;
        fixed m10 = 0;
        fixed m11 = FIXED_ONE;

        if (flags & WE_HAVE_A_SCALE) {
            if (p + 2 > font->data + font->size) {
                ttf_free_outline(out);
                return false;
            }
            int16_t s = (int16_t)be16(p);
            p += 2; // 2.14 fixed
            fixed scale = (fixed)((int32_t)s << (FIXED_SHIFT - 14));
            m00 = scale;
            m11 = scale;
        } else if (flags & WE_HAVE_AN_X_AND_Y_SCALE) {
            if (p + 4 > font->data + font->size) {
                ttf_free_outline(out);
                return false;
            }
            int16_t sx = (int16_t)be16(p);     // 2.14
            int16_t sy = (int16_t)be16(p + 2); // 2.14
            p += 4;
            m00 = (fixed)((int32_t)sx << (FIXED_SHIFT - 14));
            m11 = (fixed)((int32_t)sy << (FIXED_SHIFT - 14));
        } else if (flags & WE_HAVE_A_TWO_BY_TWO) {
            if (p + 8 > font->data + font->size) {
                ttf_free_outline(out);
                return false;
            }
            int16_t sx = (int16_t)be16(p);      // 2.14
            int16_t shy = (int16_t)be16(p + 2); // 2.14
            int16_t shx = (int16_t)be16(p + 4); // 2.14
            int16_t sy = (int16_t)be16(p + 6);  // 2.14
            p += 8;
            m00 = (fixed)((int32_t)sx << (FIXED_SHIFT - 14));
            m01 = (fixed)((int32_t)shy << (FIXED_SHIFT - 14));
            m10 = (fixed)((int32_t)shx << (FIXED_SHIFT - 14));
            m11 = (fixed)((int32_t)sy << (FIXED_SHIFT - 14));
        }

        // 递归加载子 glyph 的 outline
        TTF_GlyphOutline comp;
        memset(&comp, 0, sizeof(comp));
        if (!ttf_load_glyph_outline(font, compIndex, &comp)) {
            ttf_free_outline(out);
            return false;
        }

        uint16_t old_point_count = out->pointCount;
        uint16_t old_contour_count = out->contourCount;

        uint16_t new_point_count = old_point_count + comp.pointCount;
        uint16_t new_contour_count = old_contour_count + comp.contourCount;

        // 重新分配 points / endPts
        TTF_Point* new_points =
            (TTF_Point*)kmalloc(sizeof(TTF_Point) * new_point_count);
        uint16_t* new_endpts =
            (uint16_t*)kmalloc(sizeof(uint16_t) * new_contour_count);
        if (!new_points || !new_endpts) {
            if (new_points)
                kfree(new_points);
            if (new_endpts)
                kfree(new_endpts);
            ttf_free_outline(&comp);
            ttf_free_outline(out);
            return false;
        }

        for (uint16_t i = 0; i < old_point_count; i++) {
            new_points[i] = out->points[i];
        }
        for (uint16_t i = 0; i < old_contour_count; i++) {
            new_endpts[i] = out->endPts[i];
        }

        for (uint16_t i = 0; i < comp.pointCount; i++) {
            TTF_Point src = comp.points[i];

            fixed x = int_to_fixed(src.x);
            fixed y = int_to_fixed(src.y);

            fixed tx = fixed_mul(m00, x) + fixed_mul(m01, y) + int_to_fixed(dx);
            fixed ty = fixed_mul(m10, x) + fixed_mul(m11, y) + int_to_fixed(dy);

            TTF_Point dst;
            dst.x = (int16_t)fixed_to_int(tx);
            dst.y = (int16_t)fixed_to_int(ty);
            dst.on_curve = src.on_curve;

            new_points[old_point_count + i] = dst;
        }

        // 更新 contour endPts（加上 old_point_count 偏移）
        for (uint16_t cidx = 0; cidx < comp.contourCount; cidx++) {
            new_endpts[old_contour_count + cidx] =
                old_point_count + comp.endPts[cidx];
        }

        if (out->points)
            kfree(out->points);
        if (out->endPts)
            kfree(out->endPts);

        out->points = new_points;
        out->endPts = new_endpts;
        out->pointCount = new_point_count;
        out->contourCount = new_contour_count;

        ttf_free_outline(&comp);

        has_more = (flags & MORE_COMPONENTS) != 0;
    }

    return true;
}

static void ttf_free_outline(TTF_GlyphOutline* g) {
    if (!g)
        return;
    if (g->points)
        kfree(g->points);
    if (g->endPts)
        kfree(g->endPts);
    memset(g, 0, sizeof(*g));
}

/* ========================
 * Bézier 展开（定点）
 * ======================== */

static void bezier2_fixed(fixed x0, fixed y0, fixed cx, fixed cy, fixed x1,
                          fixed y1, Edge* edges, uint32_t* edge_count,
                          uint32_t max_edges) {
    const int STEPS = 32;
    fixed px = x0;
    fixed py = y0;

    for (int i = 1; i <= STEPS; i++) {
        fixed t = int_to_fixed(i) / STEPS;
        fixed mt = FIXED_ONE - t;

        fixed mt2 = fixed_mul(mt, mt);
        fixed t2 = fixed_mul(t, t);
        fixed two = int_to_fixed(2);

        fixed x = fixed_mul(mt2, x0) +
                  fixed_mul(fixed_mul(two, fixed_mul(mt, t)), cx) +
                  fixed_mul(t2, x1);

        fixed y = fixed_mul(mt2, y0) +
                  fixed_mul(fixed_mul(two, fixed_mul(mt, t)), cy) +
                  fixed_mul(t2, y1);

        if (*edge_count < max_edges) {
            edges[*edge_count].x0 = px;
            edges[*edge_count].y0 = py;
            edges[*edge_count].x1 = x;
            edges[*edge_count].y1 = y;
            (*edge_count)++;
        }

        px = x;
        py = y;
    }
}

/* ========================
 * 轮廓 → 边
 * ======================== */

static const TTF_Point* contour_get_pt(const TTF_Point* pts, uint16_t count,
                                       int idx) {
    if (idx >= (int)count)
        idx -= count;
    return &pts[idx];
}

static void add_contour_edges(const TTF_Point* pts, uint16_t count, Edge* edges,
                              uint32_t* edge_count, uint32_t max_edges) {
    if (count < 2)
        return;

    // 1) 把原始点复制到临时数组，并插入隐含 on-curve 点
    // 最坏情况：点数翻倍（每对 off-curve 之间插入一个），所以 *2 足够
    TTF_Point* tmp = (TTF_Point*)kmalloc(sizeof(TTF_Point) * count * 2);
    if (!tmp)
        return;

    uint16_t n = 0;

    // 处理 first/last off-curve 的情况
    TTF_Point first = pts[0];
    TTF_Point last = pts[count - 1];

    bool first_off = !first.on_curve;
    bool last_off = !last.on_curve;

    uint16_t start = 0;

    if (first_off && last_off) {
        // 在末尾和开头之间插入一个隐含 on-curve 点 = 中点
        TTF_Point mid;
        mid.x = (int16_t)((first.x + last.x) / 2);
        mid.y = (int16_t)((first.y + last.y) / 2);
        mid.on_curve = 1;
        tmp[n++] = mid;
        start = 0;
    }

    // 2) 遍历原始点，处理连续 off-curve 之间的隐含点
    for (uint16_t i = 0; i < count; i++) {
        TTF_Point p = pts[i];

        tmp[n++] = p;

        TTF_Point next = pts[(i + 1) % count];

        if (!p.on_curve && !next.on_curve) {
            // off -> off：插入中点作为隐含 on-curve
            TTF_Point mid;
            mid.x = (int16_t)((p.x + next.x) / 2);
            mid.y = (int16_t)((p.y + next.y) / 2);
            mid.on_curve = 1;
            tmp[n++] = mid;
        }
    }

    // 现在 tmp[0..n-1] 是处理过隐含点的 contour，首尾已经闭合
    // 3) 遍历 tmp，生成直线段 / 二次贝塞尔段

    TTF_Point cur = tmp[0];

    for (uint16_t i = 1; i <= n; i++) {
        TTF_Point p1 = tmp[i % n];

        if (cur.on_curve && p1.on_curve) {
            if (*edge_count < max_edges) {
                edges[*edge_count].x0 = int_to_fixed(cur.x);
                edges[*edge_count].y0 = int_to_fixed(cur.y);
                edges[*edge_count].x1 = int_to_fixed(p1.x);
                edges[*edge_count].y1 = int_to_fixed(p1.y);
                (*edge_count)++;
            }
            cur = p1;
        } else if (cur.on_curve && !p1.on_curve) {
            // on -> off -> next
            TTF_Point p2 = tmp[(i + 1) % n];
            if (!p2.on_curve) {
                // 按我们的构造，这里不会出现 off->off，因为间隔已经插入隐含点
                cur = p2;
                i++;
                continue;
            }

            fixed x0 = int_to_fixed(cur.x);
            fixed y0 = int_to_fixed(cur.y);
            fixed cx = int_to_fixed(p1.x);
            fixed cy = int_to_fixed(p1.y);
            fixed x1 = int_to_fixed(p2.x);
            fixed y1 = int_to_fixed(p2.y);

            bezier2_fixed(x0, y0, cx, cy, x1, y1, edges, edge_count, max_edges);

            cur = p2;
            i++; // 额外跳过 p2，因为已经消耗掉了
        } else if (!cur.on_curve && p1.on_curve) {
            // off -> on：按规范，这种情况只可能出现在我们构造的隐含点之后
            // 这里可以看成一条简单的二次曲线，控制点=cur，起点为前一个 on-curve
            // 但由于我们总是从 on-curve 开始构造 cur，这种情况理论上不会出现
            cur = p1;
        } else {
            // off -> off：在前面构造阶段已经消除，这里不应该出现
            cur = p1;
        }
    }

    kfree(tmp);
}

/* ========================
 * Scanline 填充（定点）
 * ======================== */

static void rasterize_edges_fill_fixed(const Edge* edges, uint32_t edge_count,
                                       int x_offset, int y_offset,
                                       uint32_t color) {
    if (edge_count == 0)
        return;

    // 计算像素空间 y 范围
    fixed y_min = edges[0].y0;
    fixed y_max = edges[0].y0;

    for (uint32_t i = 0; i < edge_count; i++) {
        if (edges[i].y0 < y_min)
            y_min = edges[i].y0;
        if (edges[i].y1 < y_min)
            y_min = edges[i].y1;
        if (edges[i].y0 > y_max)
            y_max = edges[i].y0;
        if (edges[i].y1 > y_max)
            y_max = edges[i].y1;
    }

    int iy_min = fixed_to_int(y_min) - 1;
    int iy_max = fixed_to_int(y_max) + 1;
    if (iy_min > iy_max)
        return;

    // 一行最多 edge_count 个交点，每个交点一个 x 和一个 winding delta
    fixed* xints = (fixed*)kmalloc(sizeof(fixed) * edge_count);
    int16_t* wdeltas = (int16_t*)kmalloc(sizeof(int16_t) * edge_count);
    if (!xints || !wdeltas) {
        if (xints)
            kfree(xints);
        if (wdeltas)
            kfree(wdeltas);
        return;
    }

    for (int iy = iy_min; iy <= iy_max; iy++) {
        fixed y = int_to_fixed(iy);
        int cnt = 0;

        for (uint32_t i = 0; i < edge_count; i++) {
            fixed x0 = edges[i].x0;
            fixed y0 = edges[i].y0;
            fixed x1 = edges[i].x1;
            fixed y1 = edges[i].y1;

            if (y0 == y1)
                continue;

            // 半开区间：y ∈ [min, max)
            bool intersect = ((y >= y0 && y < y1) || (y >= y1 && y < y0));

            if (!intersect)
                continue;

            // 计算交点 x
            fixed t = fixed_div((y - y0), (y1 - y0));
            fixed x = x0 + fixed_mul(t, (x1 - x0));

            // 计算 winding delta：上升边 +1，下降边 -1
            int16_t delta = (y1 > y0) ? +1 : -1;

            xints[cnt] = x;
            wdeltas[cnt] = delta;
            cnt++;
        }

        if (cnt == 0)
            continue;

        // 按 x 排序，同时保持 wdeltas 对应
        for (int i = 0; i < cnt - 1; i++) {
            for (int j = i + 1; j < cnt; j++) {
                if (xints[i] > xints[j]) {
                    fixed tx = xints[i];
                    xints[i] = xints[j];
                    xints[j] = tx;

                    int16_t td = wdeltas[i];
                    wdeltas[i] = wdeltas[j];
                    wdeltas[j] = td;
                }
            }
        }

        // non-zero winding：从左到右累积 winding
        int winding = 0;
        fixed span_start = 0;
        bool in_span = false;

        for (int i = 0; i < cnt; i++) {
            int prev_winding = winding;
            winding += wdeltas[i];

            if (prev_winding == 0 && winding != 0) {
                // 进入内部：新 span 开始
                span_start = xints[i];
                in_span = true;
            } else if (prev_winding != 0 && winding == 0 && in_span) {
                // 退出内部：span 结束
                fixed span_end = xints[i];

                int ix0 = fixed_to_int(span_start);
                int ix1 = fixed_to_int(span_end);
                if (ix0 > ix1) {
                    int tmp = ix0;
                    ix0 = ix1;
                    ix1 = tmp;
                }

                for (int ix = ix0; ix <= ix1; ix++) {
                    int sx = x_offset + ix;
                    int sy = y_offset - iy; // 字体 y 向上，屏幕 y 向下
                    put_pixel((uint32_t)sx, (uint32_t)sy, color);
                }

                in_span = false;
            }
        }
    }

    kfree(xints);
    kfree(wdeltas);
}

static void rasterize_edges_fill_fixed_fb(const Edge* edges,
                                          uint32_t edge_count, int x_offset,
                                          int y_offset, uint32_t color) {
    if (edge_count == 0)
        return;

    // 计算像素空间 y 范围
    fixed y_min = edges[0].y0;
    fixed y_max = edges[0].y0;

    for (uint32_t i = 0; i < edge_count; i++) {
        if (edges[i].y0 < y_min)
            y_min = edges[i].y0;
        if (edges[i].y1 < y_min)
            y_min = edges[i].y1;
        if (edges[i].y0 > y_max)
            y_max = edges[i].y0;
        if (edges[i].y1 > y_max)
            y_max = edges[i].y1;
    }

    int iy_min = fixed_to_int(y_min) - 1;
    int iy_max = fixed_to_int(y_max) + 1;
    if (iy_min > iy_max)
        return;

    // 一行最多 edge_count 个交点，每个交点一个 x 和一个 winding delta
    fixed* xints = (fixed*)kmalloc(sizeof(fixed) * edge_count);
    int16_t* wdeltas = (int16_t*)kmalloc(sizeof(int16_t) * edge_count);
    if (!xints || !wdeltas) {
        if (xints)
            kfree(xints);
        if (wdeltas)
            kfree(wdeltas);
        return;
    }

    for (int iy = iy_min; iy <= iy_max; iy++) {
        fixed y = int_to_fixed(iy);
        int cnt = 0;

        for (uint32_t i = 0; i < edge_count; i++) {
            fixed x0 = edges[i].x0;
            fixed y0 = edges[i].y0;
            fixed x1 = edges[i].x1;
            fixed y1 = edges[i].y1;

            if (y0 == y1)
                continue;

            // 半开区间：y ∈ [min, max)
            bool intersect = ((y >= y0 && y < y1) || (y >= y1 && y < y0));

            if (!intersect)
                continue;

            // 计算交点 x
            fixed t = fixed_div((y - y0), (y1 - y0));
            fixed x = x0 + fixed_mul(t, (x1 - x0));

            // 计算 winding delta：上升边 +1，下降边 -1
            int16_t delta = (y1 > y0) ? +1 : -1;

            xints[cnt] = x;
            wdeltas[cnt] = delta;
            cnt++;
        }

        if (cnt == 0)
            continue;

        // 按 x 排序，同时保持 wdeltas 对应
        for (int i = 0; i < cnt - 1; i++) {
            for (int j = i + 1; j < cnt; j++) {
                if (xints[i] > xints[j]) {
                    fixed tx = xints[i];
                    xints[i] = xints[j];
                    xints[j] = tx;

                    int16_t td = wdeltas[i];
                    wdeltas[i] = wdeltas[j];
                    wdeltas[j] = td;
                }
            }
        }

        // non-zero winding：从左到右累积 winding
        int winding = 0;
        fixed span_start = 0;
        bool in_span = false;

        for (int i = 0; i < cnt; i++) {
            int prev_winding = winding;
            winding += wdeltas[i];

            if (prev_winding == 0 && winding != 0) {
                // 进入内部：新 span 开始
                span_start = xints[i];
                in_span = true;
            } else if (prev_winding != 0 && winding == 0 && in_span) {
                // 退出内部：span 结束
                fixed span_end = xints[i];

                int ix0 = fixed_to_int(span_start);
                int ix1 = fixed_to_int(span_end);
                if (ix0 > ix1) {
                    int tmp = ix0;
                    ix0 = ix1;
                    ix1 = tmp;
                }

                for (int ix = ix0; ix <= ix1; ix++) {
                    int sx = x_offset + ix;
                    int sy = y_offset - iy; // 字体 y 向上，屏幕 y 向下
                    put_pixel_fb((uint32_t)sx, (uint32_t)sy, color);
                }

                in_span = false;
            }
        }
    }

    kfree(xints);
    kfree(wdeltas);
}

// 把 edges 填充到 alpha bitmap 中（0~255 灰度），使用 supersampling 抗锯齿
static void rasterize_edges_to_bitmap(const Edge* edges, uint32_t edge_count,
                                      int16_t xMin, int16_t yMin, int width,
                                      int height, uint8_t* alpha) {
    if (edge_count == 0 || !alpha || width <= 0 || height <= 0)
        return;

    const int SS = TTF_SS_FACTOR;

    int big_w = width * SS;
    int big_h = height * SS;

    // supersample 空间的 bbox
    int16_t xMin_ss = xMin * SS;
    int16_t yMin_ss = yMin * SS;

    // 为 supersample buffer 分配内存（0/255）
    uint8_t* big_alpha = (uint8_t*)kmalloc(big_w * big_h);
    if (!big_alpha)
        return;
    memset(big_alpha, 0, big_w * big_h);

    // 把 edges 放大到 supersample 空间
    Edge* edges_ss = (Edge*)kmalloc(sizeof(Edge) * edge_count);
    if (!edges_ss) {
        kfree(big_alpha);
        return;
    }
    for (uint32_t i = 0; i < edge_count; i++) {
        edges_ss[i].x0 = fixed_mul(edges[i].x0, int_to_fixed(SS));
        edges_ss[i].y0 = fixed_mul(edges[i].y0, int_to_fixed(SS));
        edges_ss[i].x1 = fixed_mul(edges[i].x1, int_to_fixed(SS));
        edges_ss[i].y1 = fixed_mul(edges[i].y1, int_to_fixed(SS));
    }

    // 一行最多 edge_count 个交点
    fixed* xints = (fixed*)kmalloc(sizeof(fixed) * edge_count);
    int16_t* wdeltas = (int16_t*)kmalloc(sizeof(int16_t) * edge_count);
    if (!xints || !wdeltas) {
        if (xints)
            kfree(xints);
        if (wdeltas)
            kfree(wdeltas);
        kfree(edges_ss);
        kfree(big_alpha);
        return;
    }

    // 在 supersample 空间里做 non-zero 填充
    int iy_ss_min = yMin_ss;
    int iy_ss_max = yMin_ss + big_h - 1;

    for (int iy = iy_ss_min; iy <= iy_ss_max; iy++) {
        fixed y = int_to_fixed(iy);
        int cnt = 0;

        for (uint32_t i = 0; i < edge_count; i++) {
            fixed x0 = edges_ss[i].x0;
            fixed y0 = edges_ss[i].y0;
            fixed x1 = edges_ss[i].x1;
            fixed y1 = edges_ss[i].y1;

            if (y0 == y1)
                continue;

            bool intersect = ((y >= y0 && y < y1) || (y >= y1 && y < y0));

            if (!intersect)
                continue;

            fixed t = fixed_div((y - y0), (y1 - y0));
            fixed x = x0 + fixed_mul(t, (x1 - x0));

            int16_t delta = (y1 > y0) ? +1 : -1;

            xints[cnt] = x;
            wdeltas[cnt] = delta;
            cnt++;
        }

        if (cnt == 0)
            continue;

        // 按 x 排序
        for (int i = 0; i < cnt - 1; i++) {
            for (int j = i + 1; j < cnt; j++) {
                if (xints[i] > xints[j]) {
                    fixed tx = xints[i];
                    xints[i] = xints[j];
                    xints[j] = tx;
                    int16_t td = wdeltas[i];
                    wdeltas[i] = wdeltas[j];
                    wdeltas[j] = td;
                }
            }
        }

        int winding = 0;
        fixed span_start = 0;
        bool in_span = false;

        for (int i = 0; i < cnt; i++) {
            int prev_winding = winding;
            winding += wdeltas[i];

            if (prev_winding == 0 && winding != 0) {
                span_start = xints[i];
                in_span = true;
            } else if (prev_winding != 0 && winding == 0 && in_span) {
                fixed span_end = xints[i];

                int ix0 = fixed_to_int(span_start);
                int ix1 = fixed_to_int(span_end);
                if (ix0 > ix1) {
                    int tmp = ix0;
                    ix0 = ix1;
                    ix1 = tmp;
                }

                // 限制在 supersample bbox
                if (ix0 < xMin_ss)
                    ix0 = xMin_ss;
                if (ix1 > xMin_ss + big_w - 1)
                    ix1 = xMin_ss + big_w - 1;

                int by_ss = iy - yMin_ss;
                if (by_ss < 0 || by_ss >= big_h)
                    continue;

                for (int ix = ix0; ix <= ix1; ix++) {
                    int bx_ss = ix - xMin_ss;
                    if (bx_ss < 0 || bx_ss >= big_w)
                        continue;
                    big_alpha[by_ss * big_w + bx_ss] = 255;
                }

                in_span = false;
            }
        }
    }

    kfree(xints);
    kfree(wdeltas);
    kfree(edges_ss);

    // 下采样：big_w x big_h -> width x height，平均得到 0~255 灰度
    for (int y0 = 0; y0 < height; y0++) {
        for (int x0 = 0; x0 < width; x0++) {
            int sum = 0;
            for (int dy = 0; dy < SS; dy++) {
                int yy = y0 * SS + dy;
                for (int dx = 0; dx < SS; dx++) {
                    int xx = x0 * SS + dx;
                    sum += big_alpha[yy * big_w + xx];
                }
            }
            int avg = sum / (SS * SS); // 0..255
            alpha[y0 * width + x0] = (uint8_t)avg;
        }
    }

    kfree(big_alpha);
}

static uint32_t alpha_blend_over(uint32_t src, uint32_t dst, uint8_t a) {
    if (a == 255)
        return src;
    if (a == 0)
        return dst;

    uint32_t sa = (src >> 24) & 0xFF;
    uint32_t sr = (src >> 16) & 0xFF;
    uint32_t sg = (src >> 8) & 0xFF;
    uint32_t sb = (src >> 0) & 0xFF;

    uint32_t dr = (dst >> 16) & 0xFF;
    uint32_t dg = (dst >> 8) & 0xFF;
    uint32_t db = (dst >> 0) & 0xFF;

    // 简单认为 src 完全不透明，用 a 作为覆盖权重
    uint32_t r = (sr * a + dr * (255 - a)) / 255;
    uint32_t g = (sg * a + dg * (255 - a)) / 255;
    uint32_t b = (sb * a + db * (255 - a)) / 255;

    return (0xFFu << 24) | (r << 16) | (g << 8) | b;
}

static void blit_glyph_bitmap(const TTF_GlyphCacheEntry* e, int x, int y,
                              uint32_t color) {
    if (!e || !e->alpha)
        return;

    int dst_x0 = x + e->xMin;
    int dst_y0 = y - e->yMin;

    for (int by = 0; by < e->height; by++) {
        for (int bx = 0; bx < e->width; bx++) {
            uint8_t a = e->alpha[by * e->width + bx];
            if (!a)
                continue;

            int sx = dst_x0 + bx;
            int sy = dst_y0 - by;

            uint32_t dst = get_pixel(sx, sy);
            uint32_t out = alpha_blend_over(color, dst, a);
            put_pixel((uint32_t)sx, (uint32_t)sy, out);
        }
    }
}

static void blit_glyph_bitmap_buf(const TTF_GlyphCacheEntry* e, int x, int y,
                                  uint32_t color, uint32_t* buf, int buf_w,
                                  int buf_h) {
    if (!e || !e->alpha || !buf)
        return;

    int dst_x0 = x + e->xMin;
    int dst_y0 = y - e->yMin;

    for (int by = 0; by < e->height; by++) {
        for (int bx = 0; bx < e->width; bx++) {
            uint8_t a = e->alpha[by * e->width + bx];
            if (!a)
                continue;

            int sx = dst_x0 + bx;
            int sy = dst_y0 - by;

            if (sx < 0 || sx >= buf_w || sy < 0 || sy >= buf_h)
                continue;

            uint32_t dst = buf[sy * buf_w + sx];
            buf[sy * buf_w + sx] = alpha_blend_over(color, dst, a);
        }
    }
}

static void blit_glyph_bitmap_fb(const TTF_GlyphCacheEntry* e, int x, int y,
                                 uint32_t color) {
    if (!e || !e->alpha)
        return;

    int dst_x0 = x + e->xMin;
    int dst_y0 = y - e->yMin;

    for (int by = 0; by < e->height; by++) {
        for (int bx = 0; bx < e->width; bx++) {
            uint8_t a = e->alpha[by * e->width + bx];
            if (!a)
                continue;

            int sx = dst_x0 + bx;
            int sy = dst_y0 - by;

            uint32_t dst = get_pixel(sx, sy);
            uint32_t out = alpha_blend_over(color, dst, a);
            put_pixel_fb((uint32_t)sx, (uint32_t)sy, out);
        }
    }
}

/* ========================
 * 对外渲染接口：单 glyph
 * ======================== */

void ttf_draw_glyph(const TTF_Font* font, uint16_t glyph_index, int x, int y,
                    int pixel_size, uint32_t color) {
    if (!font || pixel_size <= 0)
        return;

    // 1) 先查缓存
    TTF_GlyphCacheEntry* cached =
        glyph_cache_lookup(glyph_index, (uint16_t)pixel_size);
    if (cached) {
        blit_glyph_bitmap(cached, x, y, color);
        return;
    }

    // 2) 缓存没有：解析 outline
    TTF_GlyphOutline outline;
    memset(&outline, 0, sizeof(outline));
    if (!ttf_load_glyph_outline(font, glyph_index, &outline)) {
        return;
    }

    uint32_t max_edges = outline.pointCount * 64;
    Edge* edges = (Edge*)kmalloc(sizeof(Edge) * max_edges);
    if (!edges) {
        ttf_free_outline(&outline);
        return;
    }

    uint32_t edge_count = 0;
    uint16_t start = 0;
    for (uint16_t ci = 0; ci < outline.contourCount; ci++) {
        uint16_t end = outline.endPts[ci];
        uint16_t count = end - start + 1;
        add_contour_edges(&outline.points[start], count, edges, &edge_count,
                          max_edges);
        start = end + 1;
    }

    // 3) 把 edges 缩放到像素空间
    fixed scale =
        fixed_div(int_to_fixed(pixel_size), int_to_fixed(font->unitsPerEm));

    for (uint32_t i = 0; i < edge_count; i++) {
        edges[i].x0 = fixed_mul(edges[i].x0, scale);
        edges[i].y0 = fixed_mul(edges[i].y0, scale);
        edges[i].x1 = fixed_mul(edges[i].x1, scale);
        edges[i].y1 = fixed_mul(edges[i].y1, scale);
    }

    // 4) 计算像素空间 bbox（用于 cache bitmap）
    fixed x_min_f = edges[0].x0;
    fixed x_max_f = edges[0].x0;
    fixed y_min_f = edges[0].y0;
    fixed y_max_f = edges[0].y0;

    for (uint32_t i = 0; i < edge_count; i++) {
        if (edges[i].x0 < x_min_f)
            x_min_f = edges[i].x0;
        if (edges[i].x1 < x_min_f)
            x_min_f = edges[i].x1;
        if (edges[i].x0 > x_max_f)
            x_max_f = edges[i].x0;
        if (edges[i].x1 > x_max_f)
            x_max_f = edges[i].x1;

        if (edges[i].y0 < y_min_f)
            y_min_f = edges[i].y0;
        if (edges[i].y1 < y_min_f)
            y_min_f = edges[i].y1;
        if (edges[i].y0 > y_max_f)
            y_max_f = edges[i].y0;
        if (edges[i].y1 > y_max_f)
            y_max_f = edges[i].y1;
    }

    int16_t xMin = (int16_t)fixed_to_int(x_min_f);
    int16_t xMax = (int16_t)fixed_to_int(x_max_f);
    int16_t yMin = (int16_t)fixed_to_int(y_min_f);
    int16_t yMax = (int16_t)fixed_to_int(y_max_f);

    int width = xMax - xMin + 1;
    int height = yMax - yMin + 1;

    if (width <= 0 || height <= 0) {
        kfree(edges);
        ttf_free_outline(&outline);
        return;
    }

    // 5) 为 cache 分配 entry 和 bitmap
    TTF_GlyphCacheEntry* entry =
        glyph_cache_insert(glyph_index, (uint16_t)pixel_size);
    entry->xMin = xMin;
    entry->xMax = xMax;
    entry->yMin = yMin;
    entry->yMax = yMax;
    entry->width = width;
    entry->height = height;

    // bearing：基线 (x,y) 在字形坐标系是 y=0
    // bitmap 左上是 (xMin, yMax) → bearingX = xMin, bearingY = yMax
    entry->bearingX = xMin;
    entry->bearingY = yMax;

    entry->alpha = (uint8_t*)kmalloc(width * height);
    if (!entry->alpha) {
        // 回退：不用 cache，直接画到屏幕
        kfree(edges);
        ttf_free_outline(&outline);
        entry->used = false;
        return;
    }
    memset(entry->alpha, 0, width * height);

    // 6) raster 到 bitmap（non-zero winding）
    rasterize_edges_to_bitmap(edges, edge_count, xMin, yMin, width, height,
                              entry->alpha);

    // 7) 把 cache 里的 bitmap blit 到屏幕
    blit_glyph_bitmap(entry, x, y, color);

    kfree(edges);
    ttf_free_outline(&outline);
}

void ttf_draw_glyph_fb(const TTF_Font* font, uint16_t glyph_index, int x, int y,
                       int pixel_size, uint32_t color) {
    if (!font || pixel_size <= 0)
        return;

    // 1) 先查缓存
    TTF_GlyphCacheEntry* cached =
        glyph_cache_lookup(glyph_index, (uint16_t)pixel_size);
    if (cached) {
        blit_glyph_bitmap_fb(cached, x, y, color);
        return;
    }

    // 2) 缓存没有：解析 outline
    TTF_GlyphOutline outline;
    memset(&outline, 0, sizeof(outline));
    if (!ttf_load_glyph_outline(font, glyph_index, &outline)) {
        return;
    }

    uint32_t max_edges = outline.pointCount * 64;
    Edge* edges = (Edge*)kmalloc(sizeof(Edge) * max_edges);
    if (!edges) {
        ttf_free_outline(&outline);
        return;
    }

    uint32_t edge_count = 0;
    uint16_t start = 0;
    for (uint16_t ci = 0; ci < outline.contourCount; ci++) {
        uint16_t end = outline.endPts[ci];
        uint16_t count = end - start + 1;
        add_contour_edges(&outline.points[start], count, edges, &edge_count,
                          max_edges);
        start = end + 1;
    }

    // 3) 把 edges 缩放到像素空间
    fixed scale =
        fixed_div(int_to_fixed(pixel_size), int_to_fixed(font->unitsPerEm));

    for (uint32_t i = 0; i < edge_count; i++) {
        edges[i].x0 = fixed_mul(edges[i].x0, scale);
        edges[i].y0 = fixed_mul(edges[i].y0, scale);
        edges[i].x1 = fixed_mul(edges[i].x1, scale);
        edges[i].y1 = fixed_mul(edges[i].y1, scale);
    }

    // 4) 计算像素空间 bbox（用于 cache bitmap）
    fixed x_min_f = edges[0].x0;
    fixed x_max_f = edges[0].x0;
    fixed y_min_f = edges[0].y0;
    fixed y_max_f = edges[0].y0;

    for (uint32_t i = 0; i < edge_count; i++) {
        if (edges[i].x0 < x_min_f)
            x_min_f = edges[i].x0;
        if (edges[i].x1 < x_min_f)
            x_min_f = edges[i].x1;
        if (edges[i].x0 > x_max_f)
            x_max_f = edges[i].x0;
        if (edges[i].x1 > x_max_f)
            x_max_f = edges[i].x1;

        if (edges[i].y0 < y_min_f)
            y_min_f = edges[i].y0;
        if (edges[i].y1 < y_min_f)
            y_min_f = edges[i].y1;
        if (edges[i].y0 > y_max_f)
            y_max_f = edges[i].y0;
        if (edges[i].y1 > y_max_f)
            y_max_f = edges[i].y1;
    }

    int16_t xMin = (int16_t)fixed_to_int(x_min_f);
    int16_t xMax = (int16_t)fixed_to_int(x_max_f);
    int16_t yMin = (int16_t)fixed_to_int(y_min_f);
    int16_t yMax = (int16_t)fixed_to_int(y_max_f);

    int width = xMax - xMin + 1;
    int height = yMax - yMin + 1;

    if (width <= 0 || height <= 0) {
        kfree(edges);
        ttf_free_outline(&outline);
        return;
    }

    // 5) 为 cache 分配 entry 和 bitmap
    TTF_GlyphCacheEntry* entry =
        glyph_cache_insert(glyph_index, (uint16_t)pixel_size);
    entry->xMin = xMin;
    entry->xMax = xMax;
    entry->yMin = yMin;
    entry->yMax = yMax;
    entry->width = width;
    entry->height = height;

    // bearing：基线 (x,y) 在字形坐标系是 y=0
    // bitmap 左上是 (xMin, yMax) → bearingX = xMin, bearingY = yMax
    entry->bearingX = xMin;
    entry->bearingY = yMax;

    entry->alpha = (uint8_t*)kmalloc(width * height);
    if (!entry->alpha) {
        // 回退：不用 cache，直接画到屏幕
        kfree(edges);
        ttf_free_outline(&outline);
        entry->used = false;
        return;
    }
    memset(entry->alpha, 0, width * height);

    // 6) raster 到 bitmap（non-zero winding）
    rasterize_edges_to_bitmap(edges, edge_count, xMin, yMin, width, height,
                              entry->alpha);

    // 7) 把 cache 里的 bitmap blit 到屏幕
    blit_glyph_bitmap_fb(entry, x, y, color);

    kfree(edges);
    ttf_free_outline(&outline);
}

void ttf_draw_glyph_buf(const TTF_Font* font, uint16_t glyph_index, int x,
                        int y, int pixel_size, uint32_t color, uint32_t* buf,
                        int buf_w, int buf_h) {
    if (!font || pixel_size <= 0 || !buf)
        return;

    // 先尝试从缓存获取
    uint8_t* cached_bitmap = NULL;
    int cached_width, cached_height, cached_bearing_x, cached_bearing_y,
        cached_advance;

    if (ttf_cache_get_glyph(font, glyph_index, pixel_size, color,
                            &cached_bitmap, &cached_width, &cached_height,
                            &cached_bearing_x, &cached_bearing_y,
                            &cached_advance)) {
        // 缓存命中，直接使用缓存的位图
        if (cached_bitmap) {
            // 这里需要实现位图绘制逻辑
            // 暂时使用现有的缓存机制
            TTF_GlyphCacheEntry* cached =
                glyph_cache_lookup(glyph_index, (uint16_t)pixel_size);
            if (cached) {
                blit_glyph_bitmap_buf(cached, x, y, color, buf, buf_w, buf_h);
            }
        }
        return;
    }

    // 缓存未命中，使用原有逻辑渲染
    TTF_GlyphCacheEntry* cached =
        glyph_cache_lookup(glyph_index, (uint16_t)pixel_size);
    if (cached) {
        blit_glyph_bitmap_buf(cached, x, y, color, buf, buf_w, buf_h);

        // 将渲染结果添加到缓存
        if (cached->alpha) {
            ttf_cache_put_glyph(font, glyph_index, pixel_size, color,
                                cached->alpha, cached->width, cached->height,
                                cached->bearingX, cached->bearingY,
                                ttf_get_glyph_advance(font, glyph_index));
        }
        return;
    }

    TTF_GlyphOutline outline;
    memset(&outline, 0, sizeof(outline));
    if (!ttf_load_glyph_outline(font, glyph_index, &outline))
        return;

    uint32_t max_edges = outline.pointCount * 64;
    Edge* edges = (Edge*)kmalloc(sizeof(Edge) * max_edges);
    if (!edges) {
        ttf_free_outline(&outline);
        return;
    }

    uint32_t edge_count = 0;
    uint16_t start = 0;
    for (uint16_t ci = 0; ci < outline.contourCount; ci++) {
        uint16_t end = outline.endPts[ci];
        uint16_t count = end - start + 1;
        add_contour_edges(&outline.points[start], count, edges, &edge_count,
                          max_edges);
        start = end + 1;
    }

    fixed scale =
        fixed_div(int_to_fixed(pixel_size), int_to_fixed(font->unitsPerEm));

    for (uint32_t i = 0; i < edge_count; i++) {
        edges[i].x0 = fixed_mul(edges[i].x0, scale);
        edges[i].y0 = fixed_mul(edges[i].y0, scale);
        edges[i].x1 = fixed_mul(edges[i].x1, scale);
        edges[i].y1 = fixed_mul(edges[i].y1, scale);
    }

    fixed x_min_f = edges[0].x0;
    fixed x_max_f = edges[0].x0;
    fixed y_min_f = edges[0].y0;
    fixed y_max_f = edges[0].y0;

    for (uint32_t i = 0; i < edge_count; i++) {
        if (edges[i].x0 < x_min_f)
            x_min_f = edges[i].x0;
        if (edges[i].x1 < x_min_f)
            x_min_f = edges[i].x1;
        if (edges[i].x0 > x_max_f)
            x_max_f = edges[i].x0;
        if (edges[i].x1 > x_max_f)
            x_max_f = edges[i].x1;
        if (edges[i].y0 < y_min_f)
            y_min_f = edges[i].y0;
        if (edges[i].y1 < y_min_f)
            y_min_f = edges[i].y1;
        if (edges[i].y0 > y_max_f)
            y_max_f = edges[i].y0;
        if (edges[i].y1 > y_max_f)
            y_max_f = edges[i].y1;
    }

    int16_t xMin = (int16_t)fixed_to_int(x_min_f);
    int16_t xMax = (int16_t)fixed_to_int(x_max_f);
    int16_t yMin = (int16_t)fixed_to_int(y_min_f);
    int16_t yMax = (int16_t)fixed_to_int(y_max_f);

    int width = xMax - xMin + 1;
    int height = yMax - yMin + 1;

    if (width <= 0 || height <= 0) {
        kfree(edges);
        ttf_free_outline(&outline);
        return;
    }

    TTF_GlyphCacheEntry* entry =
        glyph_cache_insert(glyph_index, (uint16_t)pixel_size);
    entry->xMin = xMin;
    entry->xMax = xMax;
    entry->yMin = yMin;
    entry->yMax = yMax;
    entry->width = width;
    entry->height = height;
    entry->bearingX = xMin;
    entry->bearingY = yMax;

    entry->alpha = (uint8_t*)kmalloc(width * height);
    if (!entry->alpha) {
        kfree(edges);
        ttf_free_outline(&outline);
        entry->used = false;
        return;
    }
    memset(entry->alpha, 0, width * height);

    rasterize_edges_to_bitmap(edges, edge_count, xMin, yMin, width, height,
                              entry->alpha);

    blit_glyph_bitmap_buf(entry, x, y, color, buf, buf_w, buf_h);

    kfree(edges);
    ttf_free_outline(&outline);
}

/* ========================
 * UTF‑8 解码 + 文本渲染
 * ======================== */

// 返回 codepoint，并前移 *p
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

// 简单左到右一行 UTF‑8
void ttf_draw_text_utf8(const TTF_Font* font, int x, int y, int pixel_size,
                        uint32_t color, const char* utf8) {
    if (!font || !utf8)
        return;

    int pen_x = x;
    const char* p = utf8;

    while (*p) {
        uint32_t cp = utf8_decode(&p);
        if (cp == '\n') {
            int line_height =
                (int)((font->ascender - font->descender + font->lineGap) *
                      (int64_t)pixel_size / font->unitsPerEm);
            pen_x = x;
            y += line_height;
            continue;
        }

        uint16_t glyph = ttf_char_to_glyph(font, cp);
        if (glyph == 0) {
            uint16_t space_advance =
                ttf_get_glyph_advance(font, ttf_char_to_glyph(font, ' '));
            pen_x +=
                (int)((int64_t)space_advance * pixel_size / font->unitsPerEm);
            continue;
        }

        ttf_draw_glyph(font, glyph, pen_x, y, pixel_size, color);

        uint16_t adv = ttf_get_glyph_advance(font, glyph);
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
            int line_height =
                (int)((font->ascender - font->descender + font->lineGap) *
                      (int64_t)pixel_size / font->unitsPerEm);
            pen_x = x;
            y += line_height;
            continue;
        }

        uint16_t glyph = ttf_char_to_glyph(font, cp);
        if (glyph == 0) {
            uint16_t space_advance =
                ttf_get_glyph_advance(font, ttf_char_to_glyph(font, ' '));
            pen_x +=
                (int)((int64_t)space_advance * pixel_size / font->unitsPerEm);
            continue;
        }

        ttf_draw_glyph_fb(font, glyph, pen_x, y, pixel_size, color);

        uint16_t adv = ttf_get_glyph_advance(font, glyph);
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
            int line_height =
                (int)((font->ascender - font->descender + font->lineGap) *
                      (int64_t)pixel_size / font->unitsPerEm);
            pen_x = x;
            y += line_height;
            continue;
        }

        uint16_t glyph = ttf_char_to_glyph(font, cp);
        if (glyph == 0) {
            uint16_t space_advance =
                ttf_get_glyph_advance(font, ttf_char_to_glyph(font, ' '));
            pen_x +=
                (int)((int64_t)space_advance * pixel_size / font->unitsPerEm);
            continue;
        }

        ttf_draw_glyph_buf(font, glyph, pen_x, y, pixel_size, color, buf, buf_w,
                           buf_h);

        uint16_t adv = ttf_get_glyph_advance(font, glyph);
        pen_x += (int)((int64_t)adv * pixel_size / font->unitsPerEm);
    }
}

TTF_GlyphCacheEntry* ttf_render_glyph(TTF_Font* font, uint16_t glyph_index,
                                      uint16_t pixel_size) {
    // 查缓存
    TTF_GlyphCacheEntry* e = glyph_cache_lookup(glyph_index, pixel_size);
    if (e)
        return e;

    // 插入缓存
    e = glyph_cache_insert(glyph_index, pixel_size);

    // 加载 outline
    TTF_GlyphOutline outline;
    if (!ttf_load_glyph_outline(font, glyph_index, &outline)) {
        return NULL;
    }

    // 缩放
    fixed scale =
        fixed_div(int_to_fixed(pixel_size), int_to_fixed(font->unitsPerEm));

    e->xMin = fixed_to_int(fixed_mul(int_to_fixed(outline.xMin), scale));
    e->yMin = fixed_to_int(fixed_mul(int_to_fixed(outline.yMin), scale));
    e->xMax = fixed_to_int(fixed_mul(int_to_fixed(outline.xMax), scale));
    e->yMax = fixed_to_int(fixed_mul(int_to_fixed(outline.yMax), scale));

    e->width = e->xMax - e->xMin + 1;
    e->height = e->yMax - e->yMin + 1;

    e->bearingX = e->xMin;
    e->bearingY = e->yMax;

    // 分配 alpha bitmap
    e->alpha = kmalloc(e->width * e->height);
    memset(e->alpha, 0, e->width * e->height);

    // 生成边
    Edge* edges = kmalloc(sizeof(Edge) * 4096);
    uint32_t edge_count = 0;

    for (int c = 0; c < outline.contourCount; c++) {
        int start = (c == 0 ? 0 : outline.endPts[c - 1] + 1);
        int end = outline.endPts[c];
        add_contour_edges(outline.points + start, end - start + 1, edges,
                          &edge_count, 4096);
    }

    // raster
    rasterize_edges_fill_fixed(edges, edge_count, -e->xMin, -e->yMin,
                               0xFFFFFFFF);

    kfree(edges);
    ttf_free_outline(&outline);

    return e;
}

TTF_GlyphCacheEntry* ttf_render_glyph_fb(TTF_Font* font, uint16_t glyph_index,
                                         uint16_t pixel_size) {
    // 查缓存
    TTF_GlyphCacheEntry* e = glyph_cache_lookup(glyph_index, pixel_size);
    if (e)
        return e;

    // 插入缓存
    e = glyph_cache_insert(glyph_index, pixel_size);

    // 加载 outline
    TTF_GlyphOutline outline;
    if (!ttf_load_glyph_outline(font, glyph_index, &outline)) {
        return NULL;
    }

    // 缩放
    fixed scale =
        fixed_div(int_to_fixed(pixel_size), int_to_fixed(font->unitsPerEm));

    e->xMin = fixed_to_int(fixed_mul(int_to_fixed(outline.xMin), scale));
    e->yMin = fixed_to_int(fixed_mul(int_to_fixed(outline.yMin), scale));
    e->xMax = fixed_to_int(fixed_mul(int_to_fixed(outline.xMax), scale));
    e->yMax = fixed_to_int(fixed_mul(int_to_fixed(outline.yMax), scale));

    e->width = e->xMax - e->xMin + 1;
    e->height = e->yMax - e->yMin + 1;

    e->bearingX = e->xMin;
    e->bearingY = e->yMax;

    // 分配 alpha bitmap
    e->alpha = kmalloc(e->width * e->height);
    memset(e->alpha, 0, e->width * e->height);

    // 生成边
    Edge* edges = kmalloc(sizeof(Edge) * 4096);
    uint32_t edge_count = 0;

    for (int c = 0; c < outline.contourCount; c++) {
        int start = (c == 0 ? 0 : outline.endPts[c - 1] + 1);
        int end = outline.endPts[c];
        add_contour_edges(outline.points + start, end - start + 1, edges,
                          &edge_count, 4096);
    }

    // raster
    rasterize_edges_fill_fixed_fb(edges, edge_count, -e->xMin, -e->yMin,
                                  0xFFFFFFFF);

    kfree(edges);
    ttf_free_outline(&outline);

    return e;
}

void ttf_dump_codepoints(TTF_Font* font) {
    for (uint32_t cp = 0; cp <= 0xFFFF; cp++) {
        uint16_t g = ttf_char_to_glyph(font, cp);
        if (g != 0) {
            if (cp >= 0xE000 && cp <= 0xF8FF) {
                shell_printf("PUA U+%04X -> glyph %u\n", cp, g);
            }
        }
    }
}

void ttf_play_loading_animation(const char* font_path, int x, int y,
                                uint32_t duration_ms, uint16_t pixel_size) {
    serial_puts("ttf 1\n");
    TTF_Font* font = ttf_load_from_path(font_path);
    if (!font)
        return;
    ttf_dump_codepoints(font);
    uint16_t frames[64];
    int frame_count = 0;

    // 扫描 U+E000 ~ U+E02F
    for (uint32_t cp = 0xE052; cp <= 0xE0CB; cp++) {
        uint16_t g = ttf_char_to_glyph(font, cp);
        if (g != 0)
            frames[frame_count++] = cp;
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

        sleep_ms(100); // 100ms 一帧
        graphics_present();
    }

    ttf_unload(font);
}
