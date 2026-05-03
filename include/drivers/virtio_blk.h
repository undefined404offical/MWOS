// drivers/virtio_blk.h

#pragma once
#include <stdint.h>
#include <stdbool.h>

bool virtio_blk_init(void);
uint32_t virtio_blk_read(uint32_t lba, uint32_t count, void* buffer);
uint32_t virtio_blk_write(uint32_t lba, uint32_t count, const void* buffer);
