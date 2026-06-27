// kernel/drivers/virtio_blk.c
#include "drivers/virtio_blk.h"
#include "drivers/pci.h"
#include "io.h"
#include "serial.h"
#include "memory.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// ---------- legacy virtio PCI I/O offsets ----------
#define VIRTIO_PCI_HOST_FEATURES    0x00
#define VIRTIO_PCI_GUEST_FEATURES   0x04
#define VIRTIO_PCI_QUEUE_PFN        0x08
#define VIRTIO_PCI_QUEUE_NUM        0x0C
#define VIRTIO_PCI_QUEUE_SEL        0x0E
#define VIRTIO_PCI_QUEUE_NOTIFY     0x10
#define VIRTIO_PCI_STATUS           0x12
#define VIRTIO_PCI_ISR_STATUS       0x13
#define VIRTIO_PCI_DEVICE_CONFIG    0x14

// ---------- status bits ----------
#define VIRTIO_STATUS_ACKNOWLEDGE   1
#define VIRTIO_STATUS_DRIVER        2
#define VIRTIO_STATUS_DRIVER_OK     4
#define VIRTIO_STATUS_FEATURES_OK   8
#define VIRTIO_STATUS_FAILED        0x80

// ---------- virtqueue flags ----------
#define VIRTQ_DESC_F_NEXT   1
#define VIRTQ_DESC_F_WRITE  2

// ---------- block request types ----------
#define VIRTIO_BLK_T_IN     0
#define VIRTIO_BLK_T_OUT    1

// ---------- device config ----------
struct virtio_blk_config {
    uint64_t capacity;
    uint32_t size_max;
    uint32_t seg_max;
    uint16_t cylinders;
    uint8_t  heads;
    uint8_t  sectors;
    uint32_t blk_size;
    uint8_t  reserved[20];
} __attribute__((packed));

// ---------- virtqueue structs ----------
struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} __attribute__((packed));

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[];
} __attribute__((packed));

// ---------- request header ----------
struct virtio_blk_req {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed));

// ---------- globals ----------
static uint16_t g_io_base = 0;
static uint16_t g_queue_size = 0;

static struct virtq_desc*  g_desc  = NULL;
static struct virtq_avail* g_avail = NULL;
static struct virtq_used*  g_used  = NULL;

// ⭐⭐⭐ DMA-safe header & status
static struct virtio_blk_req* g_req_hdr = NULL;
static uint8_t* g_status_ptr = NULL;

// ---------- I/O helpers ----------
static inline uint8_t io_in8(uint16_t base, uint16_t off)   { return inb(base + off); }
static inline uint16_t io_in16(uint16_t base, uint16_t off) { return inw(base + off); }
static inline uint32_t io_in32(uint16_t base, uint16_t off) { return inl(base + off); }
static inline void io_out8(uint16_t base, uint16_t off, uint8_t v)  { outb(base + off, v); }
static inline void io_out16(uint16_t base, uint16_t off, uint16_t v){ outw(base + off, v); }
static inline void io_out32(uint16_t base, uint16_t off, uint32_t v){ outl(base + off, v); }

// ---------- init ----------
bool virtio_blk_init(void)
{
    pci_device_t dev;
    if (!pci_get_virtio_blk_device(&dev)) {
        serial_puts("VirtIO-blk: device not found\n");
        return false;
    }

    // Enable PCI Bus Master
    uint8_t bus  = dev.bus;
    uint8_t slot = dev.device;
    uint8_t func = dev.function;

    outl(PCI_CONFIG_ADDRESS,
         (1U << 31) |
         ((uint32_t)bus  << 16) |
         ((uint32_t)slot << 11) |
         ((uint32_t)func << 8)  |
         (0x04 & 0xFC));

    uint32_t cmd_data = inl(PCI_CONFIG_DATA);
    uint16_t cmd = (uint16_t)(cmd_data & 0xFFFF);

    cmd |= 0x0004;
    cmd_data = (cmd_data & 0xFFFF0000) | cmd;

    outl(PCI_CONFIG_ADDRESS,
         (1U << 31) |
         ((uint32_t)bus  << 16) |
         ((uint32_t)slot << 11) |
         ((uint32_t)func << 8)  |
         (0x04 & 0xFC));
    outl(PCI_CONFIG_DATA, cmd_data);

    // BAR0 = legacy I/O port
    uint16_t io_base = pci_get_io_bar_base(&dev, 0);
    if (io_base == 0) {
        serial_puts("VirtIO-blk: IO BAR0 not found\n");
        return false;
    }
    g_io_base = io_base;

    // Reset
    io_out8(g_io_base, VIRTIO_PCI_STATUS, 0);

    // ACK + DRIVER
    io_out8(g_io_base, VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    io_out8(g_io_base, VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    // Feature negotiation
    io_out32(g_io_base, VIRTIO_PCI_GUEST_FEATURES, 0);

    // FEATURES_OK
    io_out8(g_io_base, VIRTIO_PCI_STATUS,
            VIRTIO_STATUS_ACKNOWLEDGE |
            VIRTIO_STATUS_DRIVER |
            VIRTIO_STATUS_FEATURES_OK);

    uint8_t status = io_in8(g_io_base, VIRTIO_PCI_STATUS);
    if (!(status & VIRTIO_STATUS_FEATURES_OK)) {
        serial_puts("VirtIO-blk: FEATURES_OK rejected\n");
        return false;
    }

    // Select queue 0
    io_out16(g_io_base, VIRTIO_PCI_QUEUE_SEL, 0);

    uint16_t qsz = io_in16(g_io_base, VIRTIO_PCI_QUEUE_NUM);
    if (qsz == 0) {
        serial_puts("VirtIO-blk: queue size 0\n");
        return false;
    }

    if (qsz > 8) qsz = 8;
    g_queue_size = qsz;

io_out16(g_io_base, VIRTIO_PCI_QUEUE_NUM, g_queue_size);

    // Allocate virtqueue memory (4K aligned)
    size_t desc_size  = sizeof(struct virtq_desc) * g_queue_size;
    size_t avail_size = sizeof(struct virtq_avail) + sizeof(uint16_t) * g_queue_size;
    size_t used_size  = sizeof(struct virtq_used)  + sizeof(struct virtq_used_elem) * g_queue_size;

    const size_t VRING_ALIGN = 4096;

    size_t avail_end   = desc_size + avail_size;
    size_t used_offset = (avail_end + VRING_ALIGN - 1) & ~(VRING_ALIGN - 1);
    size_t total       = used_offset + used_size;

    uint8_t* raw = (uint8_t*)kmalloc(total + VRING_ALIGN);
    uint64_t aligned = ((uint64_t)raw + VRING_ALIGN - 1) & ~(VRING_ALIGN - 1);

    uint8_t* mem = (uint8_t*)aligned;

    g_desc  = (struct virtq_desc*)mem;
    g_avail = (struct virtq_avail*)(mem + desc_size);
    g_used  = (struct virtq_used *)(mem + used_offset);

    memset(mem, 0, total);

    // PFN
    uint32_t pfn = ((uint64_t)mem) >> 12;
    io_out32(g_io_base, VIRTIO_PCI_QUEUE_PFN, pfn);

    // DRIVER_OK
    io_out8(g_io_base, VIRTIO_PCI_STATUS,
            VIRTIO_STATUS_ACKNOWLEDGE |
            VIRTIO_STATUS_DRIVER |
            VIRTIO_STATUS_FEATURES_OK |
            VIRTIO_STATUS_DRIVER_OK);

    // ⭐⭐⭐ Allocate DMA-safe header & status
    g_req_hdr    = (struct virtio_blk_req*)kmalloc(sizeof(struct virtio_blk_req));
    g_status_ptr = (uint8_t*)kmalloc(1);

    return true;
}

// ---------- core I/O ----------
static uint32_t virtio_blk_do_io(uint32_t type,
                                 uint32_t lba,
                                 uint32_t count,
                                 void* buffer)
{
    if (count == 0)
        return 0;

    struct virtio_blk_req* req = g_req_hdr;
    uint8_t* status            = g_status_ptr;

    req->type     = type;
    req->reserved = 0;
    req->sector   = (uint64_t)lba;
    *status       = 0xFF;

    // ⭐⭐⭐ DMA-safe data buffer
    void* dma_buf = kmalloc(512 * count);
    memset(dma_buf, 0xAA, 512 * count);

    if (type == VIRTIO_BLK_T_OUT)
        memcpy(dma_buf, buffer, 512 * count);

    // desc[0]: header
    g_desc[0].addr  = (uint64_t)req;
    g_desc[0].len   = sizeof(struct virtio_blk_req);
    g_desc[0].flags = VIRTQ_DESC_F_NEXT;
    g_desc[0].next  = 1;

    // desc[1]: data
    g_desc[1].addr  = (uint64_t)dma_buf;
    g_desc[1].len   = 512U * count;
    g_desc[1].flags = VIRTQ_DESC_F_NEXT;
    if (type == VIRTIO_BLK_T_IN)
        g_desc[1].flags |= VIRTQ_DESC_F_WRITE;
    g_desc[1].next  = 2;

    // desc[2]: status
    g_desc[2].addr  = (uint64_t)status;
    g_desc[2].len   = 1;
    g_desc[2].flags = VIRTQ_DESC_F_WRITE;
    g_desc[2].next  = 0;

    // Add to avail
    uint16_t idx = g_avail->idx;
    g_avail->ring[idx % g_queue_size] = 0;
    g_avail->idx = idx + 1;

    // Notify
    io_out16(g_io_base, VIRTIO_PCI_QUEUE_NOTIFY, 0);

    // Wait
    uint16_t last_used = g_used->idx;
    uint32_t spin = 0;

    while (g_used->idx == last_used) {
        if (++spin > 200000000) {
            serial_puts("VirtIO-blk: IO timeout\n");
            return 1;
        }
        (void)io_in8(g_io_base, VIRTIO_PCI_ISR_STATUS);
    }

    if (*status != 0) {
        serial_puts("VirtIO-blk: status=");
        serial_puthex8(*status);
        serial_puts("\n");
        return 1;
    }

    if (type == VIRTIO_BLK_T_IN)
        memcpy(buffer, dma_buf, 512 * count);

    return 0;
}

// ---------- public API ----------
uint32_t virtio_blk_read(uint32_t lba, uint32_t count, void* buffer)
{
    return virtio_blk_do_io(VIRTIO_BLK_T_IN, lba, count, buffer);
}

uint32_t virtio_blk_write(uint32_t lba, uint32_t count, const void* buffer)
{
    return virtio_blk_do_io(VIRTIO_BLK_T_OUT, lba, count, (void*)buffer);
}
