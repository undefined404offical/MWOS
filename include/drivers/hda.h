#ifndef HDA_H
#define HDA_H

#include <stdint.h>
#include <stdbool.h>

// =========================
// HDA Controller Registers
// =========================

// Global registers
#define HDA_REG_GCAP        0x00
#define HDA_REG_GCTL        0x08
#define HDA_REG_STATESTS    0x0E   

// GET_PARAMETER params ID
#define HDA_PARAM_NODE_COUNT        0x04
#define HDA_PARAM_NODE_START        0x05
#define HDA_PARAM_WIDGET_CAP        0x09
#define HDA_PARAM_PIN_CAP           0x0C
#define HDA_PARAM_DEFAULT_CFG       0x1C
#define HDA_PARAM_CONN_LIST_LEN     0x0E

// CORB registers
#define HDA_REG_CORBLBASE   0x40
#define HDA_REG_CORBUBASE   0x44
#define HDA_REG_CORBWP      0x48
#define HDA_REG_CORBRP      0x4A
#define HDA_REG_CORBCTL     0x4C
#define HDA_REG_CORBSIZE    0x4E

// RIRB registers
#define HDA_REG_RIRBLBASE   0x50
#define HDA_REG_RIRBUBASE   0x54
#define HDA_REG_RIRBWP      0x58
#define HDA_REG_RINTCNT     0x5A
#define HDA_REG_RIRBCTL     0x5C
#define HDA_REG_RIRBSTS     0x5D
#define HDA_REG_RIRBSIZE    0x5E

// DMA Position Buffer
#define HDA_REG_DPLBASE     0x70
#define HDA_REG_DPUBASE     0x74

#define HDA_REG_WAKEEN      0x0C

// Stream Descriptor
#define HDA_SD_STRIDE       0x20

#define HDA_SD_CTL          0x00
#define HDA_SD_STS          0x03
#define HDA_SD_CBL          0x08
#define HDA_SD_LVI          0x0C
#define HDA_SD_FMT          0x12
#define HDA_SD_BDLPL        0x18
#define HDA_SD_BDLPU        0x1C

// =========================
// BDL Entry
// =========================
typedef struct {
    uint64_t address;
    uint32_t length;
    uint32_t flags;    // bit0 = IOC
} __attribute__((packed)) hda_bdl_t;

// =========================
// Verb construction
// =========================
#define HDA_MAKE_VERB(codec, nid, verb_id, payload) \
    ( ((uint32_t)(codec)  << 28) | \
      ((uint32_t)(nid)    << 20) | \
      ((uint32_t)(verb_id) << 8) | \
      ((uint32_t)(payload) & 0xFF) )

// =========================
// Verb IDs
// =========================
#define HDA_VERB_GET_PARAMETER         0xF00
#define HDA_VERB_SET_POWER_STATE       0x705
#define HDA_VERB_SET_STREAM_CHANNEL    0x706
#define HDA_VERB_SET_PIN_WIDGET_CTRL   0x707
#define HDA_VERB_SET_EAPD_BTLENABLE    0x70C
#define HDA_VERB_SET_CONVERTER_FORMAT  0x200

// =========================
// Typical QEMU codec nodes
// =========================
#define HDA_CODEC0        0
#define HDA_NODE_ROOT     0
#define HDA_NODE_AFG      1
#define HDA_NODE_DAC      2
#define HDA_NODE_PIN_OUT  3

// =========================
// Public API
// =========================
void hda_init(uint64_t bar0_addr);
void hda_play_pcm(void* data, uint32_t size);
void hda_test_beep(void);
void hda_test_noise();

#endif // HDA_H
