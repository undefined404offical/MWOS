// drivers/disk.c

#include "drivers/disk.h"
#include "drivers/virtio_blk.h"
#include "drivers/ide.h"
#include "serial.h"

static int g_use_virtio = 0;

void disk_init(void)
{
    /*if (virtio_blk_init()) {
        serial_puts("Disk: using VirtIO-Block\n");
        g_use_virtio = 1;
    } else {
        serial_puts("Disk: VirtIO init failed, fallback to IDE\n");
        g_use_virtio = 0;
        ide_init();
    }*/
    g_use_virtio = 0;
    ide_init();
}

uint32_t disk_read(uint32_t lba, uint32_t count, void* buffer)
{
    if (g_use_virtio) {
        return virtio_blk_read(lba, count, buffer);
    } else {
        return ide_read_sectors(lba, count, buffer);
    }
}

uint32_t disk_write(uint32_t lba, uint32_t count, const void* buffer)
{
    if (g_use_virtio) {
        return virtio_blk_write(lba, count, buffer);
    } else {
        return ide_write_sectors(lba, count, (void*)buffer);
    }
}
uint32_t g_active_disk = 0;      // 0 = IDE, 1 = VirtIO
uint32_t g_partition_lba = 0;

/*uint32_t detect_fat32_partition(void)
{
    uint8_t buffer[512];

    for (int disk = 0; disk < 2; disk++) {

        serial_puts("Checking disk ");
        serial_putdec64(disk);
        serial_puts("\n");

        int ok = 0;
        if (disk == 0)
            ok = ide_read_sectors(0, 1, buffer);
        else
            ok = virtio_blk_read(0, 1, buffer);

        if (ok != 0) {
            serial_puts("Disk read failed\n");
            continue;
        }

        if (buffer[510] != 0x55 || buffer[511] != 0xAA) {
            serial_puts("No MBR signature\n");
            continue;
        }

        uint8_t type = buffer[0x1BE + 4];
        if (type == 0x0B || type == 0x0C) {
            uint32_t start_lba = *(uint32_t*)&buffer[0x1BE + 8];

            serial_puts("FAT32 partition found on disk ");
            serial_putdec64(disk);
            serial_puts(" at LBA ");
            serial_putdec64(start_lba);
            serial_puts("\n");

            g_active_disk = disk;
            g_partition_lba = start_lba;
            return 1;
        }
    }

    serial_puts("No FAT32 partition found on any disk\n");
    return 0;
}
*/