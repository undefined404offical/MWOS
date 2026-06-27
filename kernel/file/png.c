#include <stdint.h>
#include <stddef.h>
#include "memory.h"     // kmalloc, kfree
#include "zlib.h"       // uncompress

enum {
    PNG_OK = 0,
    PNG_ERR_FORMAT = -1,
    PNG_ERR_UNSUPPORTED = -2,
    PNG_ERR_NOMEM = -3,
    PNG_ERR_ZLIB = -4
};

typedef struct {
    uint32_t width;
    uint32_t height;
    uint8_t bit_depth;
    uint8_t color_type;
    uint8_t compression;
    uint8_t filter;
    uint8_t interlace;
} png_info_t;

static uint32_t read_u32_be(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |
           (uint32_t)p[3];
}

static int png_check_signature(const uint8_t* data, size_t size) {
    static const uint8_t sig[8] = {
        0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A
    };
    if (size < 8) return 0;
    for (int i = 0; i < 8; i++) {
        if (data[i] != sig[i]) return 0;
    }
    return 1;
}

// 解析 IHDR，顺便找到第一个 chunk 之后的指针
static int png_parse_ihdr(const uint8_t* data, size_t size,
                          png_info_t* out,
                          const uint8_t** out_chunks_start,
                          const uint8_t** out_chunks_end)
{
    if (!png_check_signature(data, size)) return PNG_ERR_FORMAT;
    if (size < 8 + 25) return PNG_ERR_FORMAT; // sig + IHDR chunk 最少 25 字节

    const uint8_t* p = data + 8;
    const uint8_t* end = data + size;

    uint32_t len = read_u32_be(p); p += 4;
    if (p + 4 > end) return PNG_ERR_FORMAT;

    uint32_t type =
        ((uint32_t)p[0] << 24) |
        ((uint32_t)p[1] << 16) |
        ((uint32_t)p[2] << 8)  |
        (uint32_t)p[3];
    p += 4;

    if (type != 0x49484452) { // "IHDR"
        return PNG_ERR_FORMAT;
    }
    if (len != 13) return PNG_ERR_FORMAT;
    if (p + len + 4 > end) return PNG_ERR_FORMAT; // data + CRC

    out->width      = read_u32_be(p);
    out->height     = read_u32_be(p + 4);
    out->bit_depth  = p[8];
    out->color_type = p[9];
    out->compression= p[10];
    out->filter     = p[11];
    out->interlace  = p[12];

    if (out->bit_depth != 8) return PNG_ERR_UNSUPPORTED;
    if (!(out->color_type == 2 || out->color_type == 6)) return PNG_ERR_UNSUPPORTED;
    if (out->compression != 0 || out->filter != 0) return PNG_ERR_UNSUPPORTED;
    if (out->interlace != 0) return PNG_ERR_UNSUPPORTED;

    // 指向下一个 chunk（IHDR 的 data + CRC 后）
    p += len + 4;

    *out_chunks_start = p;
    *out_chunks_end   = end;
    return PNG_OK;
}

// 收集所有 IDAT 数据，返回拼接后的大 buffer
static uint8_t* png_collect_idat(const uint8_t* chunks_start,
                                 const uint8_t* chunks_end,
                                 size_t* out_len)
{
    const uint8_t* p = chunks_start;
    size_t total = 0;

    while (p + 8 <= chunks_end) {
        uint32_t len = read_u32_be(p); p += 4;
        if (p + 4 > chunks_end) break;
        uint32_t type =
            ((uint32_t)p[0] << 24) |
            ((uint32_t)p[1] << 16) |
            ((uint32_t)p[2] << 8)  |
            (uint32_t)p[3];
        p += 4;

        if (p + len + 4 > chunks_end) break;

        if (type == 0x49444154) { // "IDAT"
            total += len;
        }

        p += len + 4; // skip data + CRC
        if (type == 0x49454E44) { // "IEND"
            break;
        }
    }

    if (total == 0) {
        *out_len = 0;
        return NULL;
    }

    uint8_t* out = (uint8_t*)kmalloc(total);
    if (!out) {
        *out_len = 0;
        return NULL;
    }

    p = chunks_start;
    size_t offset = 0;
    while (p + 8 <= chunks_end && offset < total) {
        uint32_t len = read_u32_be(p); p += 4;
        if (p + 4 > chunks_end) break;
        uint32_t type =
            ((uint32_t)p[0] << 24) |
            ((uint32_t)p[1] << 16) |
            ((uint32_t)p[2] << 8)  |
            (uint32_t)p[3];
        p += 4;

        if (p + len + 4 > chunks_end) break;

        if (type == 0x49444154) { // "IDAT"
            // 这里假设 total 已经足够，不再检查越界
            for (uint32_t i = 0; i < len; i++) {
                out[offset + i] = p[i];
            }
            offset += len;
        }

        p += len + 4;
        if (type == 0x49454E44) { // "IEND"
            break;
        }
    }

    *out_len = total;
    return out;
}

static int png_bytes_per_pixel(uint8_t color_type, uint8_t bit_depth) {
    // 我们只支持 bit_depth == 8
    if (bit_depth != 8) return -1;
    switch (color_type) {
        case 2: return 3; // RGB
        case 6: return 4; // RGBA
        default: return -1;
    }
}

static uint8_t* png_inflate_image(const uint8_t* comp, size_t comp_len,
                                  const png_info_t* info,
                                  size_t* out_len)
{
    int bpp = png_bytes_per_pixel(info->color_type, info->bit_depth);
    if (bpp < 0) return NULL;

    size_t row_size = (size_t)1 + (size_t)info->width * (size_t)bpp;
    size_t total = row_size * (size_t)info->height;

    uint8_t* out = (uint8_t*)kmalloc(total);
    if (!out) return NULL;

    uLongf dest_len = (uLongf)total;
    int ret = uncompress(out, &dest_len, comp, (uLong)comp_len);
    if (ret != Z_OK || dest_len != total) {
        kfree(out);
        return NULL;
    }

    *out_len = total;
    return out;
}

static uint8_t paeth_predictor(uint8_t a, uint8_t b, uint8_t c) {
    int p = (int)a + (int)b - (int)c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;

    if (pa <= pb && pa <= pc) return a;
    else if (pb <= pc) return b;
    else return c;
}

// in: 解压后带 filter 的数据
// out: 去 filter 后的原始行数据（每行 width * bpp 字节）
static int png_unfilter(const uint8_t* in, uint8_t* out,
                        const png_info_t* info)
{
    int bpp = png_bytes_per_pixel(info->color_type, info->bit_depth);
    if (bpp < 0) return PNG_ERR_UNSUPPORTED;

    size_t width = info->width;
    size_t height = info->height;
    size_t stride = width * (size_t)bpp;

    const uint8_t* src = in;
    uint8_t* dst = out;
    const uint8_t* prev_row = NULL;

    for (size_t y = 0; y < height; y++) {
        uint8_t filter_type = *src++;
        uint8_t* cur_row = dst;

        for (size_t i = 0; i < stride; i++) {
            cur_row[i] = src[i];
        }

        switch (filter_type) {
            case 0: // None
                break;

            case 1: // Sub
                for (size_t x = 0; x < stride; x++) {
                    uint8_t left = (x >= (size_t)bpp) ? cur_row[x - bpp] : 0;
                    cur_row[x] = (uint8_t)(cur_row[x] + left);
                }
                break;

            case 2: // Up
                if (prev_row) {
                    for (size_t x = 0; x < stride; x++) {
                        cur_row[x] = (uint8_t)(cur_row[x] + prev_row[x]);
                    }
                }
                // 如果是第一行，prev_row == NULL，相当于加 0
                break;

            case 3: // Average
                for (size_t x = 0; x < stride; x++) {
                    uint8_t left = (x >= (size_t)bpp) ? cur_row[x - bpp] : 0;
                    uint8_t up   = prev_row ? prev_row[x] : 0;
                    uint8_t avg  = (uint8_t)(((int)left + (int)up) / 2);
                    cur_row[x] = (uint8_t)(cur_row[x] + avg);
                }
                break;

            case 4: // Paeth
                for (size_t x = 0; x < stride; x++) {
                    uint8_t left = (x >= (size_t)bpp) ? cur_row[x - bpp] : 0;
                    uint8_t up   = prev_row ? prev_row[x] : 0;
                    uint8_t upleft =
                        (prev_row && x >= (size_t)bpp) ? prev_row[x - bpp] : 0;
                    uint8_t pa = paeth_predictor(left, up, upleft);
                    cur_row[x] = (uint8_t)(cur_row[x] + pa);
                }
                break;

            default:
                return PNG_ERR_FORMAT;
        }

        src += stride;
        dst += stride;
        prev_row = cur_row;
    }

    return PNG_OK;
}

static uint8_t* png_to_rgba(const uint8_t* raw,
                            const png_info_t* info)
{
    int bpp = png_bytes_per_pixel(info->color_type, info->bit_depth);
    if (bpp < 0) return NULL;

    size_t width = info->width;
    size_t height = info->height;
    size_t in_stride = width * (size_t)bpp;
    size_t out_stride = width * 4;
    size_t total = height * out_stride;

    uint8_t* out = (uint8_t*)kmalloc(total);
    if (!out) return NULL;

    const uint8_t* src = raw;
    uint8_t* dst = out;

    for (size_t y = 0; y < height; y++) {
        const uint8_t* row_in = src;
        uint8_t* row_out = dst;

        for (size_t x = 0; x < width; x++) {
            if (info->color_type == 2) {
                // RGB
                uint8_t r = row_in[0];
                uint8_t g = row_in[1];
                uint8_t b = row_in[2];

                row_out[0] = r;
                row_out[1] = g;
                row_out[2] = b;
                row_out[3] = 255;

                row_in  += 3;
                row_out += 4;
            } else if (info->color_type == 6) {
                // RGBA
                row_out[0] = row_in[0];
                row_out[1] = row_in[1];
                row_out[2] = row_in[2];
                row_out[3] = row_in[3];

                row_in  += 4;
                row_out += 4;
            }
        }

        src += in_stride;
        dst += out_stride;
    }

    return out;
}

uint8_t* png_to_bgra(const uint8_t* raw,
                            const png_info_t* info)
{
    int bpp = png_bytes_per_pixel(info->color_type, info->bit_depth);
    if (bpp < 0) return NULL;

    size_t width = info->width;
    size_t height = info->height;
    size_t in_stride = width * (size_t)bpp;
    size_t out_stride = width * 4;
    size_t total = height * out_stride;

    uint8_t* out = (uint8_t*)kmalloc(total);
    if (!out) return NULL;

    const uint8_t* src = raw;
    uint8_t* dst = out;

    for (size_t y = 0; y < height; y++) {
        const uint8_t* row_in = src;
        uint8_t* row_out = dst;

        for (size_t x = 0; x < width; x++) {
            if (info->color_type == 2) {
                // RGB → BGRA
                uint8_t r = row_in[0];
                uint8_t g = row_in[1];
                uint8_t b = row_in[2];

                row_out[0] = b;
                row_out[1] = g;
                row_out[2] = r;
                row_out[3] = 255;

                row_in  += 3;
                row_out += 4;
            } else if (info->color_type == 6) {
                // RGBA → BGRA
                uint8_t r = row_in[0];
                uint8_t g = row_in[1];
                uint8_t b = row_in[2];
                uint8_t a = row_in[3];

                row_out[0] = b;
                row_out[1] = g;
                row_out[2] = r;
                row_out[3] = a;

                row_in  += 4;
                row_out += 4;
            }
        }

        src += in_stride;
        dst += out_stride;
    }

    return out;
}

uint32_t blend_bgra_over_bgra(uint32_t dst, uint32_t src)
{
    // 提取 BGRA 分量
    uint8_t sb =  src        & 0xFF;
    uint8_t sg = (src >>  8) & 0xFF;
    uint8_t sr = (src >> 16) & 0xFF;
    uint8_t sa = (src >> 24) & 0xFF;

    // 完全透明：不改变 dst
    if (sa == 0) return dst;
    if (sa == 255) return src;

    uint8_t db =  dst        & 0xFF;
    uint8_t dg = (dst >>  8) & 0xFF;
    uint8_t dr = (dst >> 16) & 0xFF;
    uint8_t da = (dst >> 24) & 0xFF;

    // 归一化到 [0,255]
    uint32_t inv_a = 255 - sa;

    // 颜色通道混合：out = (src*sa + dst*(255-sa)) / 255
    uint8_t ob = (uint8_t)((sb * sa + db * inv_a + 127) / 255);
    uint8_t og = (uint8_t)((sg * sa + dg * inv_a + 127) / 255);
    uint8_t or_ = (uint8_t)((sr * sa + dr * inv_a + 127) / 255);

    // alpha 也可以混：outA = sa + da*(1-sa)
    uint8_t oa = (uint8_t)((sa + da * inv_a + 127) / 255);

    return (oa << 24) | (or_ << 16) | (og << 8) | ob;
}

int png_load_bgra(const uint8_t* data, size_t size,
                  uint32_t* out_w, uint32_t* out_h,
                  uint8_t** out_argb)
{
    png_info_t info;
    const uint8_t* chunks_start;
    const uint8_t* chunks_end;

    int ret = png_parse_ihdr(data, size, &info,
                             &chunks_start, &chunks_end);
    if (ret != PNG_OK) return ret;

    size_t idat_len = 0;
    uint8_t* idat_buf = png_collect_idat(chunks_start, chunks_end, &idat_len);
    if (!idat_buf || idat_len == 0) {
        if (idat_buf) kfree(idat_buf);
        return PNG_ERR_FORMAT;
    }

    size_t inflated_len = 0;
    uint8_t* inflated = png_inflate_image(idat_buf, idat_len,
                                          &info, &inflated_len);
    kfree(idat_buf);
    if (!inflated) {
        return PNG_ERR_ZLIB;
    }

    size_t row_stride = (size_t)info.width * (size_t)png_bytes_per_pixel(info.color_type, info.bit_depth);
    size_t raw_total = (size_t)info.height * row_stride;
    uint8_t* raw = (uint8_t*)kmalloc(raw_total);
    if (!raw) {
        kfree(inflated);
        return PNG_ERR_NOMEM;
    }

    ret = png_unfilter(inflated, raw, &info);
    kfree(inflated);
    if (ret != PNG_OK) {
        kfree(raw);
        return ret;
    }

    uint8_t* argb = png_to_bgra(raw, &info);
    kfree(raw);
    if (!argb) {
        return PNG_ERR_NOMEM;
    }

    *out_w = info.width;
    *out_h = info.height;
    *out_argb = argb;
    return PNG_OK;
}

// data/size: 整个 PNG 文件在内存中的内容
// out_w/out_h: 返回宽高
// out_rgba: 返回一块 width*height*4 的 RGBA buffer，调用者负责 kfree
int png_load_rgba(const uint8_t* data, size_t size,
                  uint32_t* out_w, uint32_t* out_h,
                  uint8_t** out_rgba)
{
    png_info_t info;
    const uint8_t* chunks_start;
    const uint8_t* chunks_end;

    int ret = png_parse_ihdr(data, size, &info,
                             &chunks_start, &chunks_end);
    if (ret != PNG_OK) return ret;

    size_t idat_len = 0;
    uint8_t* idat_buf = png_collect_idat(chunks_start, chunks_end, &idat_len);
    if (!idat_buf || idat_len == 0) {
        if (idat_buf) kfree(idat_buf);
        return PNG_ERR_FORMAT;
    }

    size_t inflated_len = 0;
    uint8_t* inflated = png_inflate_image(idat_buf, idat_len,
                                          &info, &inflated_len);
    kfree(idat_buf);
    if (!inflated) {
        return PNG_ERR_ZLIB;
    }

    size_t row_stride = (size_t)info.width * (size_t)png_bytes_per_pixel(info.color_type, info.bit_depth);
    size_t raw_total = (size_t)info.height * row_stride;
    uint8_t* raw = (uint8_t*)kmalloc(raw_total);
    if (!raw) {
        kfree(inflated);
        return PNG_ERR_NOMEM;
    }

    ret = png_unfilter(inflated, raw, &info);
    kfree(inflated);
    if (ret != PNG_OK) {
        kfree(raw);
        return ret;
    }

    uint8_t* rgba = png_to_rgba(raw, &info);
    kfree(raw);
    if (!rgba) {
        return PNG_ERR_NOMEM;
    }

    *out_w = info.width;
    *out_h = info.height;
    *out_rgba = rgba;
    return PNG_OK;
}

int png_get_info(const uint8_t* data, size_t size, png_info_t* out)
{
    if (!data || size < 33)  // 8-byte sig + 25-byte IHDR chunk
        return PNG_ERR_FORMAT;

    // 检查 PNG 签名
    static const uint8_t sig[8] = {
        0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A
    };
    for (int i = 0; i < 8; i++) {
        if (data[i] != sig[i])
            return PNG_ERR_FORMAT;
    }

    const uint8_t* p = data + 8;
    const uint8_t* end = data + size;

    // 读取 IHDR 长度
    if (p + 8 > end) return PNG_ERR_FORMAT;

    uint32_t len =
        (p[0] << 24) |
        (p[1] << 16) |
        (p[2] << 8)  |
        (p[3]);
    p += 4;

    // 必须是 IHDR
    if (p[0] != 'I' || p[1] != 'H' || p[2] != 'D' || p[3] != 'R')
        return PNG_ERR_FORMAT;
    p += 4;

    if (len != 13) return PNG_ERR_FORMAT;
    if (p + 13 > end) return PNG_ERR_FORMAT;

    // 解析 IHDR
    out->width      = (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
    out->height     = (p[4] << 24) | (p[5] << 16) | (p[6] << 8) | p[7];
    out->bit_depth  = p[8];
    out->color_type = p[9];
    out->compression= p[10];
    out->filter     = p[11];
    out->interlace  = p[12];

    return PNG_OK;
}

uint32_t* scale_bgra_nearest(const uint32_t* src,
                             uint32_t src_w, uint32_t src_h,
                             uint32_t dst_w, uint32_t dst_h)
{
    if (!src || src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0)
        return NULL;

    uint32_t* out = kmalloc(dst_w * dst_h * sizeof(uint32_t));
    if (!out) return NULL;

    // 使用 16.16 固定点避免浮点
    uint32_t x_ratio = (src_w << 16) / dst_w + 1;
    uint32_t y_ratio = (src_h << 16) / dst_h + 1;

    for (uint32_t y = 0; y < dst_h; y++) {
        uint32_t sy = (y * y_ratio) >> 16;
        const uint32_t* src_row = &src[sy * src_w];
        uint32_t* dst_row = &out[y * dst_w];

        for (uint32_t x = 0; x < dst_w; x++) {
            uint32_t sx = (x * x_ratio) >> 16;
            dst_row[x] = src_row[sx];  // 直接复制 BGRA 像素
        }
    }

    return out;
}
