#pragma once
#include <stdint.h>

void disk_init(void);
uint32_t disk_read(uint32_t lba, uint32_t count, void* buffer);
uint32_t disk_write(uint32_t lba, uint32_t count, const void* buffer);
