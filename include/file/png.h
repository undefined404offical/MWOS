#ifndef PNG_H
#define PNG_H

#include <stdint.h>
#include <stddef.h>

// PNG 错误码
enum {
    PNG_OK = 0,
    PNG_ERR_FORMAT = -1,
    PNG_ERR_UNSUPPORTED = -2,
    PNG_ERR_NOMEM = -3,
    PNG_ERR_ZLIB = -4
};

// PNG 基本信息
typedef struct {
    uint32_t width;
    uint32_t height;
    uint8_t bit_depth;
    uint8_t color_type;
    uint8_t compression;
    uint8_t filter;
    uint8_t interlace;
} png_info_t;

int png_load_bgra(const uint8_t* data, size_t size,
                  uint32_t* out_w, uint32_t* out_h,
                  uint8_t** out_argb);
int png_get_info(const uint8_t* data, size_t size, png_info_t* out);

uint32_t blend_bgra_over_bgra(uint32_t dst, uint32_t src);
uint32_t* scale_bgra_nearest(const uint32_t* src,
                             uint32_t src_w, uint32_t src_h,
                             uint32_t dst_w, uint32_t dst_h);

#endif
