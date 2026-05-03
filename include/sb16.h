#ifndef SB16_H
#define SB16_H

#include <stdint.h>
#include <stdbool.h>

// SB16 默认 I/O 基址（QEMU 里是 0x220）
#define SB16_BASE_PORT       0x220

bool sb16_init(void);
// 播放内存中的 8-bit unsigned PCM，单声道，指定采样率
bool sb16_play_8bit_mono(const uint8_t* data, uint32_t length, uint16_t sample_rate);

#endif
