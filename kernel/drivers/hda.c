#include "drivers/hda.h"
#include "memory.h"     // kmalloc, kfree
#include "serial.h"     // serial_puts, serial_puthex32, serial_putdec64"
#include "timer.h"
#include "string.h"

static void* hda_mmio = NULL;

static uint32_t* corb = NULL;
static uint64_t* rirb = NULL;
static uint16_t corb_entries = 0;
static uint16_t rirb_entries = 0;

static void* corb_mem = NULL;
static void* rirb_mem = NULL;

static void* bdl_mem = NULL;
static hda_bdl_t* hda_bdl = NULL;
static void* pcm_mem = NULL;
static uint32_t pcm_bytes = 0;

// ---- QEMU hda-duplex 简单硬编码拓扑 ----
// codec 0:
//   node 2: DAC (converter)
//   node 3: Line-out / Speaker pin

#define HDA_CODEC_ID           0
#define HDA_NODE_DAC_OUT       2
#define HDA_NODE_PIN_OUT       3

// verb 常量（按 HDA 规范）
#define HDA_VERB_GET_PARAMETER           0xF00
#define HDA_VERB_SET_STREAM_CHANNEL      0x706
#define HDA_VERB_SET_PIN_WIDGET_CONTROL  0x707
#define HDA_VERB_SET_AMP_GAIN_MUTE       0x300
#define HDA_VERB_SET_EAPD_BTLENABLE      0x70C

static void hda_delay(void)
{
    for (volatile int i = 0; i < 100000; i++) { }
}

static uint32_t hda_send_verb(uint8_t codec, uint8_t nid,
                              uint16_t verb_id, uint8_t payload);

// 初始化 CORB/RIRB，必须在 GCTL 上电后调用
static void hda_init_corb_rirb(void)
{
    uint8_t* mm = (uint8_t*)hda_mmio;

    // ----- CORB -----
    volatile uint8_t* corbsize = mm + HDA_REG_CORBSIZE;

    // 选择 256 entries（如果支持），否则退回 16 / 2
    uint8_t v = *corbsize;
    if (v & 0x04) {               // 支持 256
        v = (v & ~0x03u) | 0x02u;
        corb_entries = 256;
    } else if (v & 0x02) {        // 支持 16
        v = (v & ~0x03u) | 0x01u;
        corb_entries = 16;
    } else {                      // 否则 2
        v = (v & ~0x03u) | 0x00u;
        corb_entries = 2;
    }
    *corbsize = v;

    corb_mem = kmalloc(corb_entries * 4 + 128);
    corb = (uint32_t*)(((uintptr_t)corb_mem + 127) & ~((uintptr_t)127));

    uint64_t corb_phys = (uint64_t)(uintptr_t)corb;
    *(volatile uint32_t*)(mm + HDA_REG_CORBLBASE) = (uint32_t)corb_phys;
    *(volatile uint32_t*)(mm + HDA_REG_CORBUBASE) = (uint32_t)(corb_phys >> 32);

    // 复位 CORB RP：写 1 触发，写 0 结束 reset
    volatile uint16_t* corbrp = (volatile uint16_t*)(mm + HDA_REG_CORBRP);
    *corbrp = (1u << 15);
    *corbrp = 0;

    // CORBWP 从 0 开始
    *(volatile uint16_t*)(mm + HDA_REG_CORBWP) = 0;

    // 启用 CORB DMA
    *(volatile uint8_t*)(mm + HDA_REG_CORBCTL) |= 0x02u; // DMA enable

    // ----- RIRB -----
    volatile uint8_t* rirbsize = mm + HDA_REG_RIRBSIZE;

    v = *rirbsize;
    if (v & 0x04) {               // 256 entries
        v = (v & ~0x03u) | 0x02u;
        rirb_entries = 256;
    } else if (v & 0x02) {        // 16 entries
        v = (v & ~0x03u) | 0x01u;
        rirb_entries = 16;
    } else {
        v = (v & ~0x03u) | 0x00u; // 2 entries
        rirb_entries = 2;
    }
    *rirbsize = v;

    rirb_mem = kmalloc(rirb_entries * 8 + 128);
    rirb = (uint64_t*)(((uintptr_t)rirb_mem + 127) & ~((uintptr_t)127));

    uint64_t rirb_phys = (uint64_t)(uintptr_t)rirb;
    *(volatile uint32_t*)(mm + HDA_REG_RIRBLBASE) = (uint32_t)rirb_phys;
    *(volatile uint32_t*)(mm + HDA_REG_RIRBUBASE) = (uint32_t)(rirb_phys >> 32);

    // 复位 RIRB WP：写 1 再写 0
    volatile uint16_t* rirbwp = (volatile uint16_t*)(mm + HDA_REG_RIRBWP);
    *rirbwp = (1u << 15);
    *rirbwp = 0;

    // 每 1 条响应触发一次“新条目”
    *(volatile uint16_t*)(mm + HDA_REG_RINTCNT) = 1;

    // 清 RIRB 状态（RINT + OVERRUN）
    *(volatile uint8_t*)(mm + HDA_REG_RIRBSTS) = 0x05u;

    // 启用 RIRB DMA + RINT
    *(volatile uint8_t*)(mm + HDA_REG_RIRBCTL) |= 0x03u;
}

// 最小 send_verb：按 RIRBSTS.bit0 等响应
static uint32_t hda_send_verb(uint8_t codec, uint8_t nid,
                              uint16_t verb_id, uint8_t payload)
{
    if (!hda_mmio || !corb || !rirb)
        return 0xFFFFFFFFu;

    uint8_t* mm = (uint8_t*)hda_mmio;

    volatile uint16_t* corbwp  = (volatile uint16_t*)(mm + HDA_REG_CORBWP);
    volatile uint8_t*  corbctl = (volatile uint8_t*)(mm + HDA_REG_CORBCTL);
    volatile uint8_t*  rirbctl = (volatile uint8_t*)(mm + HDA_REG_RIRBCTL);
    volatile uint8_t*  rirbsts = (volatile uint8_t*)(mm + HDA_REG_RIRBSTS);
    volatile uint16_t* rirbwp  = (volatile uint16_t*)(mm + HDA_REG_RIRBWP);

    serial_puts("HDA: send_verb pre CORBCTL=0x"); serial_puthex8(*corbctl);
    serial_puts(" RIRBCTL=0x"); serial_puthex8(*rirbctl);
    serial_puts(" RIRBSTS=0x"); serial_puthex8(*rirbsts);
    serial_puts(" RIRBWP=0x"); serial_puthex16(*rirbwp);
    serial_puts("\n");

    uint16_t wp      = *corbwp & 0xFFu;
    uint16_t next_wp = (uint16_t)((wp + 1) % corb_entries);

    uint32_t verb = HDA_MAKE_VERB(codec, nid, verb_id, payload);
    corb[next_wp] = verb;
    *corbwp       = next_wp;

    // 等待 RIRBSTS.bit0 = 1（Response Interrupt）
    int timeout = 1000000;
    while (timeout-- > 0) {
        uint8_t sts = *rirbsts;
        if (sts & 0x01)
            break;
    }

    if (timeout <= 0) {
        serial_puts("HDA: send_verb timeout(wait sts), RIRBSTS=0x");
        serial_puthex8(*rirbsts);
        serial_puts(" RIRBWP=0x");
        serial_puthex16(*rirbwp);
        serial_puts("\n");
        return 0xFFFFFFFFu;
    }

    // 有新条目：读当前 RIRBWP 指向的 entry
    uint16_t idx = *rirbwp & 0xFFu;
    if (idx >= rirb_entries)
        idx %= rirb_entries;

    uint64_t entry = rirb[idx];
    uint32_t resp  = (uint32_t)(entry & 0xFFFFFFFFu);

    // 清 RIRB 状态（RINT + OVERRUN）
    *rirbsts = 0x05u;

    return resp;
}

// ---- 简单 codec 初始化：把 stream1 接到 DAC node2，再接到 pin node3 ----
static void hda_init_codec_simple(void)
{
    serial_puts("HDA: simple codec init for QEMU\n");

    // 1. 给 DAC node 2 绑定 stream id=1, channel=0
    uint8_t stream_id = 1;
    uint8_t channel   = 0;
    uint8_t sc_payload = (uint8_t)((stream_id << 4) | (channel & 0x0F));
    uint32_t resp;

    resp = hda_send_verb(HDA_CODEC_ID, HDA_NODE_DAC_OUT,
                         HDA_VERB_SET_STREAM_CHANNEL, sc_payload);
    serial_puts("HDA: SET_STREAM_CHANNEL(DAC2) resp=0x");
    serial_puthex32(resp);
    serial_puts("\n");

    // 2. 取消 DAC 的静音（输出放大器）
    resp = hda_send_verb(HDA_CODEC_ID, HDA_NODE_DAC_OUT,
                         HDA_VERB_SET_AMP_GAIN_MUTE, 0x00);
    serial_puts("HDA: SET_AMP_GAIN_MUTE(DAC2) resp=0x");
    serial_puthex32(resp);
    serial_puts("\n");

    // 3. 把 pin node3 设置为输出（0x40）
    resp = hda_send_verb(HDA_CODEC_ID, HDA_NODE_PIN_OUT,
                         HDA_VERB_SET_PIN_WIDGET_CONTROL, 0x40);
    serial_puts("HDA: SET_PIN_WIDGET_CONTROL(PIN3=out) resp=0x");
    serial_puthex32(resp);
    serial_puts("\n");

    // 4. 取消 pin 的静音
    resp = hda_send_verb(HDA_CODEC_ID, HDA_NODE_PIN_OUT,
                         HDA_VERB_SET_AMP_GAIN_MUTE, 0x00);
    serial_puts("HDA: SET_AMP_GAIN_MUTE(PIN3) resp=0x");
    serial_puthex32(resp);
    serial_puts("\n");

    // 5. 打开 EAPD/BTL（部分实现需要；QEMU 一般无所谓，但打开更稳）
    resp = hda_send_verb(HDA_CODEC_ID, HDA_NODE_PIN_OUT,
                         HDA_VERB_SET_EAPD_BTLENABLE, 0x02);
    serial_puts("HDA: SET_EAPD_BTLENABLE(PIN3) resp=0x");
    serial_puthex32(resp);
    serial_puts("\n");
}

// 初始化 HDA 控制器 + CORB/RIRB + codec + 测试读 Vendor ID
void hda_init(uint64_t bar0_addr)
{
    hda_mmio = (void*)(uintptr_t)bar0_addr;
    uint8_t* mm = (uint8_t*)hda_mmio;

    serial_puts("HDA: init, MMIO base=0x");
    serial_puthex64(bar0_addr);
    serial_puts("\n");

    uint16_t gcap0   = *(volatile uint16_t*)(mm + 0x00); // GCAP
    uint32_t gctl0   = *(volatile uint32_t*)(mm + 0x08); // GCTL
    uint16_t stat0   = *(volatile uint16_t*)(mm + 0x0E); // STATESTS

    serial_puts("HDA: BEFORE reset GCAP=0x"); serial_puthex16(gcap0);
    serial_puts(" GCTL=0x"); serial_puthex32(gctl0);
    serial_puts(" STATESTS=0x"); serial_puthex16(stat0);
    serial_puts("\n");

    volatile uint32_t* gctl = (volatile uint32_t*)(mm + HDA_REG_GCTL);

    // 1. 全局复位
    *gctl &= ~1u;
    while (*gctl & 1u) { }
    *gctl |= 1u;
    while (!(*gctl & 1u)) { }

    uint32_t gctl1 = *(volatile uint32_t*)(mm + 0x08);
    uint16_t stat1 = *(volatile uint16_t*)(mm + 0x0E);

    serial_puts("HDA: AFTER reset GCTL=0x"); serial_puthex32(gctl1);
    serial_puts(" STATESTS=0x"); serial_puthex16(stat1);
    serial_puts("\n");

    // 2. 初始化 CORB/RIRB
    hda_init_corb_rirb();

    // 3. 读 Vendor ID（codec0 node0）
    uint32_t vendor_resp = hda_send_verb(HDA_CODEC_ID, 0,
                                         HDA_VERB_GET_PARAMETER, 0x00);
    serial_puts("HDA: codec0 node0 VENDOR_ID resp=0x");
    serial_puthex32(vendor_resp);
    serial_puts("\n");

    // 4. 简单 codec 路由：stream1 -> DAC2 -> PIN3
    hda_init_codec_simple();
}

// 播放一段 PCM buffer
void hda_play_pcm(void* data, uint32_t size)
{
    if (!hda_mmio)
        return;

    uint8_t* mm = (uint8_t*)hda_mmio;

    // 使用第一个 output stream：假定 index=0
    uint32_t stream_index = 0;
    uint16_t gcap = *(volatile uint16_t*)(mm + HDA_REG_GCAP);
    uint16_t iss  = (gcap >> 8) & 0x0F;

    // output streams 从 0x80 + ISS*0x20 开始
    uint32_t oss_base = 0x80 + (iss * HDA_SD_STRIDE);
    uint8_t* sd = mm + oss_base + stream_index * HDA_SD_STRIDE;

    volatile uint32_t* sdctl   = (volatile uint32_t*)(sd + HDA_SD_CTL);
    volatile uint16_t* sdsts   = (volatile uint16_t*)(sd + HDA_SD_STS);
    volatile uint32_t* sdcbl   = (volatile uint32_t*)(sd + HDA_SD_CBL);
    volatile uint16_t* sdlvi   = (volatile uint16_t*)(sd + HDA_SD_LVI);
    volatile uint16_t* sdfmt   = (volatile uint16_t*)(sd + HDA_SD_FMT);
    volatile uint32_t* sdbdlpl = (volatile uint32_t*)(sd + HDA_SD_BDLPL);
    volatile uint32_t* sdbdlpu = (volatile uint32_t*)(sd + HDA_SD_BDLPU);

    // 1. 确保流停止：清 RUN
    *sdctl &= ~0x02u;

    // 2. 发 SRST 复位流
    *sdctl |= 0x01u;          // 置 SRST
    while (!(*sdctl & 0x01u)); // 等待置位
    *sdctl &= ~0x01u;         // 清 SRST
    while (*sdctl & 0x01u);   // 等待清零，流完全 idle

    // 3. 清状态位
    *sdsts = 0x1Fu;

    // 4. 释放旧 BDL
    if (bdl_mem) {
        kfree(bdl_mem);
        bdl_mem = NULL;
        hda_bdl = NULL;
    }

    // 5. 构造多条 BDL（每条最多 64KB，按 128 字节对齐）
    #define BDL_MAX_CHUNK  (64 * 1024)   // 每个 BDL entry 最大 64KB
    #define BDL_ALIGN      128           // HDA 规范要求 128 字节对齐

    uint32_t total = size;
    uint32_t max_chunk = BDL_MAX_CHUNK;
    int entries = (total + max_chunk - 1) / max_chunk;
    if (entries < 1)
        entries = 1;

    // 分配 bdl_mem：entries * sizeof(hda_bdl_t) + 对齐余量
    bdl_mem = kmalloc(entries * sizeof(hda_bdl_t) + BDL_ALIGN);
    if (!bdl_mem) {
        serial_puts("HDA: kmalloc BDL failed\n");
        return;
    }
    hda_bdl = (hda_bdl_t*)(((uintptr_t)bdl_mem + (BDL_ALIGN - 1)) &
                           ~((uintptr_t)(BDL_ALIGN - 1)));

    uint32_t offset = 0;
    for (int i = 0; i < entries; i++) {
        uint32_t remain = total - offset;
        uint32_t chunk = (remain > max_chunk) ? max_chunk : remain;

        // 向下对齐到 128 字节，避免越界
        uint32_t aligned_chunk = chunk & ~(BDL_ALIGN - 1);
        if (aligned_chunk == 0) {
            // 最后一小段不足 128 字节，直接用原始长度
            aligned_chunk = chunk;
        }

        hda_bdl[i].address = (uint64_t)(uintptr_t)((uint8_t*)data + offset);
        hda_bdl[i].length  = aligned_chunk;
        hda_bdl[i].flags   = 0x00;   // 默认不 IOC

        offset += aligned_chunk;
    }

    // 如果因为对齐丢了一点尾巴，修正最后一条长度，使总和 == size
    if (offset < total) {
        uint32_t diff = total - offset;
        hda_bdl[entries - 1].length += diff;
        offset += diff;
    }

    // 最后一条设置 IOC
    hda_bdl[entries - 1].flags = 0x01; // IOC

    // 6. 写 BDL base
    uint64_t bdl_phys = (uint64_t)(uintptr_t)hda_bdl;
    *sdbdlpl = (uint32_t)bdl_phys;
    *sdbdlpu = (uint32_t)(bdl_phys >> 32);

    // 7. 设置 CBL/LVI
    *sdcbl = size;                    // 总长度
    *sdlvi = (uint16_t)(entries - 1); // LVI = 最后一个 entry 的索引

    // 调试：打印 BDL 信息
    serial_puts("HDA: BDL entries=");
    serial_putdec64(entries);
    serial_puts(" total=");
    serial_putdec64(size);
    serial_puts(" offset=");
    serial_putdec64(offset);
    serial_puts("\n");

    // 8. 设置格式：44.1kHz, 16-bit, stereo（按规范：BASE=1, BITS=001, CHAN=0001）
    *sdfmt = 0x4011;

    // 9. 设置 stream id = 1（SD_CTL bits 23:20）
    uint32_t ctl = *sdctl;
    ctl &= ~(0xFu << 20);
    ctl |= (1u << 20);
    *sdctl = ctl;

    // 调试：RUN 前 dump 一次寄存器
    serial_puts("HDA: before RUN sdbdlpl=0x");
    serial_puthex32(*sdbdlpl);
    serial_puts(" sdcbl=");
    serial_putdec64(*sdcbl);
    serial_puts(" sdlvi=0x");
    serial_puthex16(*sdlvi);
    serial_puts(" sdfmt=0x");
    serial_puthex16(*sdfmt);
    serial_puts(" sdctl=0x");
    serial_puthex32(*sdctl);
    serial_puts(" sdsts=0x");
    serial_puthex16(*sdsts);
    serial_puts("\n");

    // 10. RUN=1，开始播放
    *sdctl |= 0x02u;

    serial_puts("HDA: playback started, size=");
    serial_putdec64(size);
    serial_puts(" bytes, entries=");
    serial_putdec64(entries);
    serial_puts("\n");
    
    volatile uint32_t* sdlpib = (volatile uint32_t*)(sd + 0x04);

    // 11. 等待 IOC
    int timeout = 2000000;
    while (timeout-- > 0) {
        if (*sdsts & 0x04u)   // IOC bit
            break;
    
        // 可选：每隔一段时间打印一次位置
        if ((timeout % 200000) == 0) {
            serial_puts("HDA: LPIB=");
            serial_putdec64(*sdlpib);
            serial_puts("\n");
        }
    }
    serial_puts("HDA: final LPIB=");
    serial_putdec64(*sdlpib);
    serial_puts("\n");
    
    // 调试：RUN 后 dump 一次寄存器
    serial_puts("HDA: after IOC/timeout sdctl=0x");
    serial_puthex32(*sdctl);
    serial_puts(" sdsts=0x");
    serial_puthex16(*sdsts);
    serial_puts(" sdcbl=");
    serial_putdec64(*sdcbl);
    serial_puts("\n");

    // 12. 停止 RUN
    *sdctl &= ~0x02u;

    // 13. 清状态位
    *sdsts = 0x1Fu;

    serial_puts("HDA: stream stopped\n");
}

static void hda_stop_stream(void)
{
    uint8_t* mm = (uint8_t*)hda_mmio;

    uint32_t stream_index = 0;
    uint16_t gcap = *(volatile uint16_t*)(mm + HDA_REG_GCAP);
    uint16_t iss = (gcap >> 8) & 0x0F;

    uint32_t oss_base = 0x80 + (iss * HDA_SD_STRIDE);
    uint8_t* sd = mm + oss_base + stream_index * HDA_SD_STRIDE;

    volatile uint32_t* sdctl = (volatile uint32_t*)(sd + HDA_SD_CTL);
    volatile uint16_t* sdsts = (volatile uint16_t*)(sd + HDA_SD_STS);

    // 停止 RUN
    *sdctl &= ~0x02u;

    *sdsts = 0x1Fu;

    serial_puts("HDA: stream stopped\n");
}

void hda_test_beep(void)
{
    if (!hda_mmio) {
        serial_puts("HDA: not initialized\n");
        return;
    }

    uint32_t sample_rate = 44100;
    uint32_t duration_ms = 500;  // 半秒
    uint32_t frames = sample_rate * duration_ms / 1000;
    uint32_t bytes = frames * 4; // stereo 16-bit

    if (pcm_mem)
        kfree(pcm_mem);

    pcm_mem = kmalloc(bytes);
    pcm_bytes = bytes;

    int16_t amp_hi = 20000;
    int16_t amp_lo = -20000;
    uint32_t freq = 440;
    uint32_t period = sample_rate / freq;
    if (period < 2) period = 2;
    uint32_t half = period / 2;

    uint8_t* buf = (uint8_t*)pcm_mem;

    for (uint32_t i = 0; i < frames; i++) {
        uint32_t pos = i % period;
        int16_t s = (pos < half) ? amp_hi : amp_lo;

        int16_t left  = s;
        int16_t right = s;

        uint32_t off = i * 4;
        buf[off + 0] = (uint8_t)(left & 0xFF);
        buf[off + 1] = (uint8_t)((left >> 8) & 0xFF);
        buf[off + 2] = (uint8_t)(right & 0xFF);
        buf[off + 3] = (uint8_t)((right >> 8) & 0xFF);
    }

    serial_puts("HDA: test beep buffer ready\n");

    hda_play_pcm(pcm_mem, pcm_bytes);
}

void hda_test_noise()
{
    // 44.1kHz, 16-bit stereo, 1秒: 44100 * 4 = 176400 字节
    uint32_t bytes = 44100 * 4;
    uint8_t* buf = (uint8_t*)kmalloc(bytes);
    if (!buf) {
        serial_puts("HDA_TEST: kmalloc failed\n");
        return;
    }

    // 填满非零数据：简单递增模式
    for (uint32_t i = 0; i < bytes; i++) {
        buf[i] = (uint8_t)(i & 0xFF);
    }

    serial_puts("HDA_TEST: playing noise, bytes=");
    serial_putdec64(bytes);
    serial_puts("\n");

    uint64_t t0 = timer_get_ticks();
    hda_play_pcm(buf, bytes);
    uint64_t t1 = timer_get_ticks();

    serial_puts("HDA_TEST: actual playback time=");
    serial_putdec64((uint32_t)(t1 - t0));
    serial_puts(" ms\n");

    kfree(buf);
}
