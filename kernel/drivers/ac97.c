#include "drivers/ac97.h"
#include "io.h"
#include "serial.h"
#include "memory.h"
#include <stdint.h>
#include <stdbool.h>

// 由 PCI 初始化时填入
static uint16_t ac97_mixer_io = 0;
static uint16_t ac97_bus_io   = 0;

// Bus Master 寄存器偏移（相对于 BAR1）
#define AC97_PO_BDBAR   0x10    // PCM Out Buffer Descriptor Base Address
#define AC97_PO_CIV     0x14    // Current Index Value
#define AC97_PO_LVI     0x15    // Last Valid Index
#define AC97_PO_SR      0x16    // Status Register
#define AC97_PO_PICB    0x18    // Position in Current Buffer
#define AC97_PO_CR      0x1B    // Control Register

// Mixer 寄存器偏移（相对于 BAR0）
#define AC97_RESET              0x00
#define AC97_MASTER_VOLUME      0x02
#define AC97_PCM_OUT_VOLUME     0x18
#define AC97_EXT_AUDIO_ID       0x28
#define AC97_EXT_AUDIO_CTRL     0x2A
#define AC97_PCM_FRONT_DAC_RATE 0x2C

// 一个简单的 BDL：32 个描述符，每个 8 字节
#define AC97_BDL_ENTRIES    32

static uint8_t* ac97_pcm_buffer = NULL;

static uint32_t ac97_pcm_frames = 0;   // 帧数（每帧=左右各16bit = 4字节）
static uint32_t ac97_pcm_buffer_bytes = 0;

typedef struct {
    uint32_t buffer_addr;
    uint16_t buffer_len;   // 先 length
    uint16_t control;      // 再 control
} __attribute__((packed)) ac97_bdl_entry_t;

static ac97_bdl_entry_t* ac97_bdl = NULL;

// 放在低端内存就行，一般 PCI DMA 能访问整个 32bit 空间
//static ac97_bdl_entry_t* ac97_bdl = NULL;
//static uint8_t* ac97_pcm_buffer = NULL;
//static uint32_t ac97_pcm_buffer_bytes = 0;
// 简单 I/O：mixer 寄存器读写（16-bit）
static inline void ac97_mixer_write(uint8_t reg, uint16_t val)
{
    outw(ac97_mixer_io + reg, val);
}

static inline uint16_t ac97_mixer_read(uint8_t reg)
{
    return inw(ac97_mixer_io + reg);
}

// Bus master I/O
static inline void ac97_bus_writeb(uint8_t reg, uint8_t val)
{
    outb(ac97_bus_io + reg, val);
}

static inline void ac97_bus_writew(uint8_t reg, uint16_t val)
{
    outw(ac97_bus_io + reg, val);
}

static inline void ac97_bus_writel(uint8_t reg, uint32_t val)
{
    outl(ac97_bus_io + reg, val);
}

static inline uint16_t ac97_bus_readw(uint8_t reg)
{
    return inw(ac97_bus_io + reg);
}

static inline uint8_t ac97_bus_readb(uint8_t reg)
{
    return inb(ac97_bus_io + reg);
}

static void ac97_delay(void)
{
    for (volatile int i = 0; i < 100000; i++) { }
}

// 软复位 codec
static bool ac97_reset_codec(void)
{
    serial_puts("AC97: reset codec...\n");

    // 写 RESET 寄存器 0，然后稍等
    ac97_mixer_write(AC97_RESET, 0x0000);
    ac97_delay();

    // 读 RESET 寄存器：bit0=1 表示 codec ready
    uint16_t v = ac97_mixer_read(AC97_RESET);
    serial_puts("AC97: RESET reg = 0x");
    serial_puthex64(v);
    serial_puts("\n");

    uint16_t vid = ac97_mixer_read(0x7C);
    uint16_t did = ac97_mixer_read(0x7E);
    serial_puts("AC97: Codec ID = 0x");
    serial_puthex16(vid);
    serial_puts(" 0x");
    serial_puthex16(did);
    serial_puts("\n");

    // 通常 v != 0 说明 ok
    return true;
}

// 设置主音量、PCM 音量（0x0000 最大，0x1F1F 静音）
static void ac97_set_volume(void)
{
    // 主音量：左/右 0x0000 = 0dB（最大）
    ac97_mixer_write(AC97_MASTER_VOLUME, 0x0000);

    // PCM 输出音量：也设最大
    ac97_mixer_write(AC97_PCM_OUT_VOLUME, 0x0000);
}

// 设置采样率（PCM 前端 DAC）
static void ac97_set_sample_rate(uint16_t rate)
{
    ac97_mixer_write(AC97_PCM_FRONT_DAC_RATE, rate);
    uint16_t r2 = ac97_mixer_read(AC97_PCM_FRONT_DAC_RATE);

    serial_puts("AC97: PCM front DAC rate=");
    serial_putdec64(r2);
    serial_puts("\n");
}

// 初始化 Bus Master，BDL + 缓冲区
// 初始化 Bus Master，BDL + 缓冲区
__attribute__((noinline))
static void ac97_setup_bdl_and_buffer(void)
{
    // 不要一上来就 1 秒，那样样本数会超出 16bit 范围
    // 先用 4096 帧（stereo），每帧 2 个 16-bit sample，共 8192 samples
    uint32_t frames_per_buffer = 4096;          // 4096 stereo frames
    uint32_t sample_rate = 48000;

    ac97_pcm_frames = frames_per_buffer;
    ac97_pcm_buffer_bytes = ac97_pcm_frames * 4; // 16-bit stereo: 4 bytes per frame

    // 分配 BDL 和 PCM 缓冲区
    ac97_bdl = (ac97_bdl_entry_t*)kmalloc(sizeof(ac97_bdl_entry_t) * AC97_BDL_ENTRIES);
    ac97_pcm_buffer = (uint8_t*)kmalloc(ac97_pcm_buffer_bytes);

    // 清空 BDL
    for (int i = 0; i < AC97_BDL_ENTRIES; i++) {
        ac97_bdl[i].buffer_addr = 0;
        ac97_bdl[i].buffer_len  = 0;
        ac97_bdl[i].control     = 0;
    }

    // AC97/ICH 规范里，buffer_len 单位是“16-bit sample 数量减 1”
    // stereo → 每帧 2 个 16-bit sample
    uint32_t samples_per_buffer = frames_per_buffer * 2; // 4096 frames * 2 = 8192 samples
    if (samples_per_buffer > 0x10000)
        samples_per_buffer = 0x10000; // 保护一下，理论上现在不会触发

    uint16_t bdl_len = (uint16_t)(samples_per_buffer - 1);  // N-1 samples

    // BDL[0]
    ac97_bdl[0].buffer_addr = (uint32_t)ac97_pcm_buffer;
    ac97_bdl[0].buffer_len  = bdl_len;
    ac97_bdl[0].control     = (1 << 15); // IOC

    // BDL[1] 复制同样内容，做双 buffer 循环
    ac97_bdl[1] = ac97_bdl[0];

    // 写 BDBAR
    ac97_bus_writel(AC97_PO_BDBAR, (uint32_t)ac97_bdl);

    // 调试：读回 BDBAR
    uint32_t bdbar_read = inl(ac97_bus_io + AC97_PO_BDBAR);
    serial_puts("AC97: BDBAR write=0x");
    serial_puthex32((uint32_t)ac97_bdl);
    serial_puts(" read=0x");
    serial_puthex32(bdbar_read);
    serial_puts("\n");

    // LVI = 1（表示 0 和 1 两个 entry 都有效，循环）
    ac97_bus_writeb(AC97_PO_LVI, 0x01);
}

// 生成一个 440Hz 方波填充 PCM 缓冲
static void ac97_fill_test_tone(void)
{
    uint32_t sample_rate = 48000;
    uint32_t period = sample_rate / 440; // 440Hz
    if (period < 2) period = 2;
    uint32_t half = period / 2;

    int16_t amp_hi = 20000;
    int16_t amp_lo = -20000;

    for (uint32_t i = 0; i < ac97_pcm_frames; i++) {
        uint32_t pos = i % period;
        int16_t s = (pos < half) ? amp_hi : amp_lo;

        int16_t left  = s;
        int16_t right = s;

        uint32_t off = i * 4;

        ac97_pcm_buffer[off + 0] = (uint8_t)(left & 0xFF);
        ac97_pcm_buffer[off + 1] = (uint8_t)((left >> 8) & 0xFF);
        ac97_pcm_buffer[off + 2] = (uint8_t)(right & 0xFF);
        ac97_pcm_buffer[off + 3] = (uint8_t)((right >> 8) & 0xFF);
    }
}

static void ac97_start_playback(void)
{
    // 1. 停止 DMA：清 RUN/AUTO
    uint8_t cr = ac97_bus_readb(AC97_PO_CR);
    cr &= ~0x03;
    ac97_bus_writeb(AC97_PO_CR, cr);

    uint8_t cr_stop = ac97_bus_readb(AC97_PO_CR);
    serial_puts("AC97: stop CR write=0x");
    serial_puthex8(cr);
    serial_puts(" read=0x");
    serial_puthex8(cr_stop);
    serial_puts("\n");

    // 2. 读 SR，清掉所有 pending 位（写 1 清）
    uint16_t sr0 = ac97_bus_readw(AC97_PO_SR);
    serial_puts("AC97: SR before clear=0x");
    serial_puthex16(sr0);
    serial_puts("\n");

    // 按规范：把读到的值原样写回，清 SR 中所有置位的 bit
    ac97_bus_writew(AC97_PO_SR, sr0);

    uint16_t sr1 = ac97_bus_readw(AC97_PO_SR);
    serial_puts("AC97: SR after clear=0x");
    serial_puthex16(sr1);
    serial_puts("\n");

    // 3. 看 CIV/LVI
    uint8_t civ = ac97_bus_readb(AC97_PO_CIV);
    uint8_t lvi = ac97_bus_readb(AC97_PO_LVI);
    serial_puts("AC97: CIV=");
    serial_putdec64(civ);
    serial_puts(" LVI=");
    serial_putdec64(lvi);
    serial_puts("\n");

    // 4. 启动：RUN=1, AUTO=1
    cr = ac97_bus_readb(AC97_PO_CR);
    cr &= ~0x03;
    cr |= 0x03;
    ac97_bus_writeb(AC97_PO_CR, cr);

    uint8_t cr_start = ac97_bus_readb(AC97_PO_CR);
    serial_puts("AC97: start CR write=0x");
    serial_puthex8(cr);
    serial_puts(" read=0x");
    serial_puthex8(cr_start);
    serial_puts("\n");

    // 5. 观察几次状态
    for (int i = 0; i < 5; i++) {
        uint16_t sr   = ac97_bus_readw(AC97_PO_SR);
        uint16_t picb = ac97_bus_readw(AC97_PO_PICB);
        uint8_t  cr_s = ac97_bus_readb(AC97_PO_CR);

        serial_puts("AC97: SR=0x");
        serial_puthex16(sr);
        serial_puts(" PICB=");
        serial_putdec64(picb);
        serial_puts(" CR=0x");
        serial_puthex8(cr_s);
        serial_puts("\n");

        ac97_delay();
    }

    serial_puts("AC97: playback started\n");
}


static void ac97_probe_bus_master_regs(void)
{
    serial_puts("AC97: probing bus master regs...\n");

    for (uint8_t off = 0x10; off < 0x20; off += 2) {
        uint16_t before = inw(ac97_bus_io + off);
        outw(ac97_bus_io + off, 0xFFFF);
        uint16_t after  = inw(ac97_bus_io + off);

        serial_puts("  off=0x");
        serial_puthex8(off);
        serial_puts(" before=0x");
        serial_puthex16(before);
        serial_puts(" after=0x");
        serial_puthex16(after);
        serial_puts("\n");
    }
}

// 对外接口：从 PCI 初始化 AC97
bool ac97_init_from_pci(uint8_t bus, uint8_t dev, uint8_t func,
                        uint32_t bar0, uint32_t bar1)
{
    (void)bus; (void)dev; (void)func;

    ac97_mixer_io = (uint16_t)bar0;
    ac97_bus_io   = (uint16_t)bar1;

    serial_puts("AC97: mixer IO=0x");
    serial_puthex64(ac97_mixer_io);
    serial_puts(" bus IO=0x");
    serial_puthex64(ac97_bus_io);
    serial_puts("\n");

    if (!ac97_mixer_io || !ac97_bus_io) {
        serial_puts("AC97: invalid BARs\n");
        return false;
    }

    if (!ac97_reset_codec()) {
        serial_puts("AC97: codec reset failed\n");
        return false;
    }

    ac97_set_volume();
    ac97_set_sample_rate(48000);

    ac97_setup_bdl_and_buffer();
    ac97_fill_test_tone();
    ac97_start_playback();

    return true;
}

void ac97_test_beep(void)
{
    if (!ac97_mixer_io || !ac97_bus_io) {
        serial_puts("AC97: not initialized\n");
        return;
    }
    ac97_fill_test_tone();
    ac97_start_playback();
}
