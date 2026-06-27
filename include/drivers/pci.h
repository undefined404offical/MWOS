#ifndef PCI_H
#define PCI_H

#include <stdint.h>
#include <stdbool.h>

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

typedef struct {
    uint8_t  bus;
    uint8_t  device;
    uint8_t  function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  revision;
    uint8_t  header_type;
} pci_device_t;

uint32_t pci_read_config32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
uint16_t pci_read_config16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
uint8_t  pci_read_config8 (uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);

void pci_scan_bus(void);
uint32_t pci_get_ide_bus_master_base(void);

bool pci_get_virtio_blk_device(pci_device_t* out_dev);
uint64_t pci_get_bar_mmio_base(const pci_device_t* dev, int bar_index);

uint32_t pci_read_config32(uint8_t bus, uint8_t device,
                           uint8_t function, uint8_t offset);
uint16_t pci_read_config16(uint8_t bus, uint8_t device,
                           uint8_t function, uint8_t offset);
uint8_t  pci_read_config8(uint8_t bus, uint8_t device,
                          uint8_t function, uint8_t offset);

void     pci_scan_bus(void);

bool     pci_get_virtio_blk_device(pci_device_t* out_dev);
uint16_t pci_get_io_bar_base(const pci_device_t* dev, int bar_index);
void pci_write_config32(uint8_t bus, uint8_t device, uint8_t function,
                        uint8_t offset, uint32_t value);
void pci_write_config16(uint8_t bus, uint8_t device, uint8_t function,
                        uint8_t offset, uint16_t value);


#endif // PCI_H
