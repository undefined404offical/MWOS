ENTRY(kmain)

SECTIONS
{
    . = 0x100000;

    _kernel_phys_start = .;   /* 内核物理起点 */

    .text : { *(.text*) }
    .rodata : ALIGN(4) { *(.rodata*) }
    .data : ALIGN(4) { *(.data*) }

    .bss : ALIGN(4) {
        *(.bss*)
        *(COMMON)
    }

    _kernel_phys_end = .;     /* 内核物理终点 */

    . = ALIGN(4096);
    _end = .;
}
