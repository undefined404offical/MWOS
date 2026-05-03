#include "drivers/pci.h"
#include "io.h"
#include "serial.h"
#include "drivers/ac97.h"
#include "drivers/hda.h"

static pci_device_t g_virtio_blk_dev;
static bool         g_virtio_blk_found = false;

static uint32_t pci_make_address(uint8_t bus, uint8_t device,
                                 uint8_t function, uint8_t offset)
{
    return (uint32_t)(
        (1U << 31) |
        ((uint32_t)bus << 16) |
        ((uint32_t)device << 11) |
        ((uint32_t)function << 8) |
        (offset & 0xFC)
    );
}

uint32_t pci_read_config32(uint8_t bus, uint8_t device,
                           uint8_t function, uint8_t offset)
{
    uint32_t address = pci_make_address(bus, device, function, offset);
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_read_config16(uint8_t bus, uint8_t device,
                           uint8_t function, uint8_t offset)
{
    uint32_t data = pci_read_config32(bus, device, function, offset & 0xFC);
    uint8_t shift = (offset & 2) * 8;
    return (uint16_t)((data >> shift) & 0xFFFF);
}

uint8_t pci_read_config8(uint8_t bus, uint8_t device,
                         uint8_t function, uint8_t offset)
{
    uint32_t data = pci_read_config32(bus, device, function, offset & 0xFC);
    uint8_t shift = (offset & 3) * 8;
    return (uint8_t)((data >> shift) & 0xFF);
}

void pci_write_config32(uint8_t bus, uint8_t device, uint8_t function,
                        uint8_t offset, uint32_t value)
{
    outl(PCI_CONFIG_ADDRESS,
         (1U << 31) |
         ((uint32_t)bus << 16) |
         ((uint32_t)device << 11) |
         ((uint32_t)function << 8) |
         (offset & 0xFC));

    outl(PCI_CONFIG_DATA, value);
}

void pci_write_config16(uint8_t bus, uint8_t device, uint8_t function,
                        uint8_t offset, uint16_t value)
{
    uint32_t old = pci_read_config32(bus, device, function, offset & 0xFC);

    uint32_t shift = (offset & 2) * 8;
    uint32_t mask  = 0xFFFF << shift;

    uint32_t newval = (old & ~mask) | ((uint32_t)value << shift);

    pci_write_config32(bus, device, function, offset, newval);
}

void pci_write_config8(uint8_t bus, uint8_t device, uint8_t function,
                       uint8_t offset, uint8_t value)
{
    uint32_t old = pci_read_config32(bus, device, function, offset & 0xFC);

    uint32_t shift = (offset & 3) * 8;
    uint32_t mask  = 0xFF << shift;

    uint32_t newval = (old & ~mask) | ((uint32_t)value << shift);

    pci_write_config32(bus, device, function, offset, newval);
}

static void pci_print_device(const pci_device_t* dev)
{
    serial_puts("[PCI] bus=");
    serial_putdec64(dev->bus);
    serial_puts(" dev=");
    serial_putdec64(dev->device);
    serial_puts(" func=");
    serial_putdec64(dev->function);

    serial_puts(" vendor=0x");
    serial_puthex16(dev->vendor_id);
    serial_puts(" device=0x");
    serial_puthex16(dev->device_id);

    serial_puts(" class=0x");
    serial_puthex8(dev->class_code);
    serial_puts(" sub=0x");
    serial_puthex8(dev->subclass);
    serial_puts(" prog_if=0x");
    serial_puthex8(dev->prog_if);

    serial_puts("\n");
}

static void pci_scan_function(uint8_t bus, uint8_t device, uint8_t function)
{
    uint16_t vendor = pci_read_config16(bus, device, function, 0x00);
    if (vendor == 0xFFFF)
        return;

    pci_device_t dev;
    dev.bus        = bus;
    dev.device     = device;
    dev.function   = function;
    dev.vendor_id  = vendor;
    dev.device_id  = pci_read_config16(bus, device, function, 0x02);
    dev.class_code = pci_read_config8(bus, device, function, 0x0B);
    dev.subclass   = pci_read_config8(bus, device, function, 0x0A);
    dev.prog_if    = pci_read_config8(bus, device, function, 0x09);
    dev.revision   = pci_read_config8(bus, device, function, 0x08);
    dev.header_type= pci_read_config8(bus, device, function, 0x0E);

    pci_print_device(&dev);

    // AC97 = class 0x04, subclass 0x01
    if (dev.class_code == 0x04 && dev.subclass == 0x01)
    {
        serial_puts("[AC97] detected, enabling PCI features...\n");

        uint16_t cmd = pci_read_config16(bus, device, function, 0x04);
        if (!(cmd & 0x0004)) {
            serial_puts("[AC97] enabling PCI Bus Master\n");
            cmd |= 0x0004;
            pci_write_config16(bus, device, function, 0x04, cmd);
        }

        uint32_t ac97_cfg40 = pci_read_config32(bus, device, function, 0x40);
        serial_puts("[AC97] PCI cfg[0x40] = 0x");
        serial_puthex32(ac97_cfg40);
        serial_puts("\n");

        uint8_t actl = pci_read_config8(bus, device, function, 0x41);
        serial_puts("[AC97] PCI audio ctrl(0x41) before = 0x");
        serial_puthex8(actl);
        serial_puts("\n");

        actl |= 0x01;  // AC97 Enable
        pci_write_config8(bus, device, function, 0x41, actl);

        uint8_t actl_after = pci_read_config8(bus, device, function, 0x41);
        serial_puts("[AC97] PCI audio ctrl(0x41) after  = 0x");
        serial_puthex8(actl_after);
        serial_puts("\n");

        uint32_t bar0 = pci_read_config32(bus, device, function, 0x10) & ~0x3U;
        uint32_t bar1 = pci_read_config32(bus, device, function, 0x14) & ~0x3U;

        serial_puts("[AC97] BAR0=0x");
        serial_puthex32(bar0);
        serial_puts(" BAR1=0x");
        serial_puthex32(bar1);
        serial_puts("\n");

        ac97_init_from_pci(bus, device, function, bar0, bar1);
    }

    // HDA = class 0x04, subclass 0x03
    if (dev.class_code == 0x04 && dev.subclass == 0x03)
    {
        serial_puts("[HDA] Intel High Definition Audio detected!\n");

        // 打开 PCI Command 的 Bus Master 位（HDA 也需要）
        uint16_t cmd = pci_read_config16(bus, device, function, 0x04);
        if (!(cmd & 0x0004)) {
            serial_puts("[HDA] enabling PCI Bus Master\n");
            cmd |= 0x0004;
            pci_write_config16(bus, device, function, 0x04, cmd);
        }

        // 读 BAR0（MMIO），屏蔽低 4 位属性
        uint32_t bar0 = pci_read_config32(bus, device, function, 0x10);
        if (bar0 == 0 || bar0 == 0xFFFFFFFF) {
            serial_puts("[HDA] invalid BAR0\n");
        } else {
            uint64_t mmio_base = (uint64_t)(bar0 & ~0x0FU);

            serial_puts("[HDA] BAR0 (MMIO) = 0x");
            serial_puthex64(mmio_base);
            serial_puts("\n");

            // 初始化 HDA 控制器并试着 beep 一下
            hda_init(mmio_base);
            //hda_test_beep();
        }
    }

    /*if (dev.header_type & 0x80) {
        for (uint8_t func = 1; func < 8; func++)
            pci_scan_function(bus, device, func);
    }*/
}

static void pci_scan_device(uint8_t bus, uint8_t device)
{
    uint16_t vendor = pci_read_config16(bus, device, 0, 0x00);
    if (vendor == 0xFFFF)
        return;
serial_puts("PCI: dev check bus="); serial_putdec64(bus); serial_puts(" dev="); serial_putdec64(device); serial_puts("\n");
    uint8_t header_type = pci_read_config8(bus, device, 0, 0x0E);

    pci_scan_function(bus, device, 0);

    if (header_type & 0x80) {
        for (uint8_t func = 1; func < 8; func++) {
            pci_scan_function(bus, device, func);
        }
    }
}

void pci_scan_bus(void)
{
    static bool scanned = false;
    if (scanned) {
        return;
    }
    scanned = true;

    serial_puts("PCI: enter pci_scan_bus()\n");

    uint8_t bus = 0;  // 你的 QEMU 环境里只有 bus 0 有设备

    serial_puts("PCI: scanning bus 0\n");

    for (uint8_t dev = 0; dev < 32; dev++) {
        pci_scan_device(bus, dev);
    }

    serial_puts("PCI: leave pci_scan_bus()\n");
}

uint32_t pci_get_ide_bus_master_base(void)
{
    uint8_t bus = 0;
    uint8_t dev = 1;
    uint8_t func = 1;

    uint16_t vendor = pci_read_config16(bus, dev, func, 0x00);
    if (vendor == 0xFFFF) {
        serial_puts("PCI: IDE device not found at 0:1:1\n");
        return 0;
    }

    // 1) 打开 Bus Master 位
    uint16_t cmd = pci_read_config16(bus, dev, func, 0x04);
    if (!(cmd & 0x0004)) {
        serial_puts("PCI: enabling Bus Master bit\n");
        cmd |= 0x0004;
        // 写回 Command
        outl(PCI_CONFIG_ADDRESS,
             (1U << 31) |
             ((uint32_t)bus << 16) |
             ((uint32_t)dev << 11) |
             ((uint32_t)func << 8) |
             (0x04 & 0xFC));
        outl(PCI_CONFIG_DATA, cmd);
    }

    // 2) 读取 BAR4
    uint32_t bar4 = pci_read_config32(bus, dev, func, 0x20);
    if (bar4 == 0 || bar4 == 0xFFFFFFFF) {
        serial_puts("PCI: IDE BAR4 invalid\n");
        return 0;
    }

    uint32_t base = bar4 & ~0x3U;

    serial_puts("PCI: IDE BusMaster base = 0x");
    serial_puthex32(base);
    serial_puts("\n");

    return base;
}

uint64_t pci_get_bar_mmio_base(const pci_device_t* dev, int bar_index)
{
    if (!dev || bar_index < 0 || bar_index > 5)
        return 0;

    uint8_t bus  = dev->bus;
    uint8_t slot = dev->device;
    uint8_t func = dev->function;

    uint8_t offset = 0x10 + bar_index * 4;
    uint32_t bar_low  = pci_read_config32(bus, slot, func, offset);

    if (bar_low == 0 || bar_low == 0xFFFFFFFF)
        return 0;

    // IO vs MMIO 判断
    if (bar_low & 0x1) {
        // I/O port BAR，不是 MMIO
        return 0;
    }

    uint32_t type = (bar_low >> 1) & 0x3;
    if (type == 0x0) {
        // 32-bit MMIO
        uint64_t base = bar_low & ~0xFU;
        return base;
    } else if (type == 0x2) {
        // 64-bit MMIO，占用两个 BAR
        uint32_t bar_high = pci_read_config32(bus, slot, func, offset + 4);
        uint64_t base = ((uint64_t)bar_high << 32) | (bar_low & ~0xFU);
        return base;
    } else {
        return 0;
    }
}

bool pci_get_virtio_blk_device(pci_device_t* out_dev)
{
    if (!g_virtio_blk_found)
        return false;
    if (out_dev)
        *out_dev = g_virtio_blk_dev;
    return true;
}

uint16_t pci_get_io_bar_base(const pci_device_t* dev, int bar_index)
{
    if (!dev || bar_index < 0 || bar_index > 5)
        return 0;

    uint8_t bus  = dev->bus;
    uint8_t slot = dev->device;
    uint8_t func = dev->function;

    uint8_t offset = 0x10 + bar_index * 4;
    uint32_t bar   = pci_read_config32(bus, slot, func, offset);
    if (bar == 0 || bar == 0xFFFFFFFF)
        return 0;

    if ((bar & 0x1) == 0) {
        // 不是 IO BAR，是 MMIO；legacy virtio-blk 我们只管 IO BAR
        return 0;
    }

    uint16_t base = (uint16_t)(bar & ~0x3U); // 低2位是属性位
    return base;
}
