#ifndef AC97_H
#define AC97_H

#include <stdint.h>
#include <stdbool.h>

bool ac97_init_from_pci(uint8_t bus, uint8_t dev, uint8_t func,
                        uint32_t bar0, uint32_t bar1);
void ac97_test_beep(void);

#endif
