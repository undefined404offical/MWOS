#include "file/wav.h"
#include "serial.h"
#include <string.h>
#include "drivers/fs/fat32.h"
#include "memory.h"
#include "drivers/hda.h"
#include "timer.h"

bool wav_parse_header(const uint8_t* buf, uint32_t size, wav_info_t* out)
{
    if (!buf || !out || size < 44)
        return false;

    // 检查 "RIFF" + "WAVE"
    if (memcmp(buf + 0, "RIFF", 4) != 0 ||
        memcmp(buf + 8, "WAVE", 4) != 0) {
        serial_puts("WAV: not RIFF/WAVE\n");
        return false;
    }

    uint32_t pos = 12; // 从第一个 chunk 开始
    uint16_t audio_format    = 0;
    uint16_t num_channels    = 0;
    uint32_t sample_rate     = 0;
    uint16_t bits_per_sample = 0;
    uint32_t data_offset     = 0;
    uint32_t data_size       = 0;

    while (pos + 8 <= size) {
        const uint8_t* chunk = buf + pos;
        char id[5] = {0};
        memcpy(id, chunk, 4);
        uint32_t chunk_size = *(const uint32_t*)(chunk + 4);

        pos += 8;
        if (pos + chunk_size > size) {
            serial_puts("WAV: chunk size overflow\n");
            return false;
        }

        if (memcmp(id, "fmt ", 4) == 0) {
            if (chunk_size < 16) {
                serial_puts("WAV: fmt chunk too small\n");
                return false;
            }
            const uint8_t* p = buf + pos;
            audio_format    = *(const uint16_t*)(p + 0);
            num_channels    = *(const uint16_t*)(p + 2);
            sample_rate     = *(const uint32_t*)(p + 4);
            bits_per_sample = *(const uint16_t*)(p + 14);

            serial_puts("WAV: fmt audio_format=");
            serial_puthex32(audio_format);
            serial_puts(" channels=");
            serial_puthex32(num_channels);
            serial_puts(" sample_rate=");
            serial_putdec64(sample_rate);
            serial_puts(" bits=");
            serial_puthex32(bits_per_sample);
            serial_puts("\n");
        } else if (memcmp(id, "data", 4) == 0) {
            data_offset = pos;
            data_size   = chunk_size;

            serial_puts("WAV: data chunk at ");
            serial_puthex32(data_offset);
            serial_puts(" size=");
            serial_putdec64(data_size);
            serial_puts("\n");
        }

        pos += chunk_size;
        if (chunk_size & 1)
            pos++; // padding
    }

    if (audio_format != 1) {
        serial_puts("WAV: only PCM supported\n");
        return false;
    }
    if (bits_per_sample != 16) {
        serial_puts("WAV: only 16-bit supported\n");
        return false;
    }
    if (num_channels != 1 && num_channels != 2) {
        serial_puts("WAV: only mono/stereo supported\n");
        return false;
    }
    if (data_offset == 0 || data_size == 0) {
        serial_puts("WAV: data chunk not found\n");
        return false;
    }

    out->audio_format    = audio_format;
    out->num_channels    = num_channels;
    out->sample_rate     = sample_rate;
    out->bits_per_sample = bits_per_sample;
    out->data_offset     = data_offset;
    out->data_size       = data_size;
    return true;
}

void hda_play_wav_file(const char* path)
{
    if (!fat32_mounted()) {
        serial_puts("WAV: FAT32 not mounted\n");
        return;
    }

    if (!fat32_file_exists(path)) {
        serial_puts("WAV: file not found: ");
        serial_puts(path);
        serial_puts("\n");
        return;
    }

    uint32_t file_size = fat32_get_file_size(path);
    if (file_size == 0) {
        serial_puts("WAV: empty file\n");
        return;
    }

    serial_puts("WAV: open ");
    serial_puts(path);
    serial_puts(" size=");
    serial_putdec64(file_size);
    serial_puts("\n");

    fat32_handle_t handle;
    if (!fat32_open(path, &handle, FILE_READ)) {
        serial_puts("WAV: fat32_open failed\n");
        return;
    }

    uint8_t* file_buf = (uint8_t*)kmalloc(file_size);
    //memset(file_buf, 0, file_size); 
    if (!file_buf) {
        serial_puts("WAV: kmalloc failed\n");
        fat32_close(&handle);
        return;
    }

    bool ok = fat32_read_all_fast(&handle, file_buf);
    fat32_close(&handle);

    if (!ok) {
        serial_puts("WAV: read_all_fast failed\n");
        kfree(file_buf);
        return;
    }

    wav_info_t info;
    if (!wav_parse_header(file_buf, file_size, &info)) {
        serial_puts("WAV: parse header failed\n");
        kfree(file_buf);
        return;
    }

    if (info.sample_rate != 44100 ||
        info.bits_per_sample != 16 ||
        info.num_channels != 2) {
        serial_puts("WAV: only 44.1kHz 16-bit stereo supported for now\n");
        kfree(file_buf);
        return;
    }

uint8_t* pcm_data = file_buf + info.data_offset;
uint32_t pcm_size = info.data_size;

uint32_t bytes_per_100ms = info.sample_rate * info.num_channels * (info.bits_per_sample / 8) / 10;
if (bytes_per_100ms > pcm_size) bytes_per_100ms = pcm_size;

uint32_t head_nonzero = 0, tail_nonzero = 0;

for (uint32_t i = 0; i < bytes_per_100ms; i++) {
    if (pcm_data[i] != 0) head_nonzero++;
}
for (uint32_t i = 0; i < bytes_per_100ms; i++) {
    if (pcm_data[pcm_size - 1 - i] != 0) tail_nonzero++;
}

serial_puts("WAV: head_nonzero=");
serial_putdec64(head_nonzero);
serial_puts(" tail_nonzero=");
serial_putdec64(tail_nonzero);
serial_puts("\n");

    //uint8_t* pcm_data = file_buf + info.data_offset;
    //uint32_t pcm_size = info.data_size;

    serial_puts("WAV: ready, pcm_size=");
    serial_putdec64(pcm_size);
    serial_puts("\n");

    uint32_t bytes_per_sec = info.sample_rate * info.num_channels * (info.bits_per_sample / 8);
    uint32_t expected_ms = (info.data_size * 1000) / bytes_per_sec;
    
    serial_puts("WAV: expected duration ~");
    serial_putdec64(expected_ms);
    serial_puts(" ms\n");
    
    uint64_t t0 = timer_get_ticks();
    hda_play_pcm(pcm_data, pcm_size);
    uint64_t t1 = timer_get_ticks();
    
    serial_puts("WAV: actual playback time=");
    serial_putdec64((uint32_t)(t1 - t0));
    serial_puts(" ms\n");



    kfree(file_buf);
}
