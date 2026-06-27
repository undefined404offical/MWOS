#ifndef WAV_H
#define WAV_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint16_t audio_format;    // 1 = PCM
    uint16_t num_channels;    // 1 or 2
    uint32_t sample_rate;     // 44100, 48000, etc.
    uint16_t bits_per_sample; // 16
    uint32_t data_offset;     // 文件内 PCM 数据起始偏移
    uint32_t data_size;       // PCM 数据字节数
} wav_info_t;

bool wav_parse_header(const uint8_t* buf, uint32_t size, wav_info_t* out);
void hda_play_wav_file(const char* path);

#endif
