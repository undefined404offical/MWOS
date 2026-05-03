#ifndef BOOT_SPLASH_H
#define BOOT_SPLASH_H

#include <stdint.h>

#define BOOT_SPLASH_MAX_LINES 32
#define BOOT_SPLASH_LINE_LEN 128

void boot_splash_init(void);
void boot_splash_log(const char *msg, uint32_t color);
void boot_splash_set_progress(int percent);
void boot_splash_present(void);

#endif
