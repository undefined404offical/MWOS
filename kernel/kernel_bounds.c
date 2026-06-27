#include <stdint.h>

// 放在 .text 最前面（紧跟代码开头）
__attribute__((used, section(".text")))
uint8_t _kernel_phys_start;

// 放在 .bss 最后面（紧跟 BSS 末尾）
__attribute__((used, section(".bss")))
uint8_t _kernel_phys_end;
