ENTRY(kmain)

SECTIONS
{
    . = 0x100000;

    _kernel_phys_start = .;

    .text : { *(.text*) }
    .rodata : ALIGN(4) { *(.rodata*) }
    .data : ALIGN(4) { *(.data*) }

    .usertest : ALIGN(4096) {
        usertest_start = .;
        *(.usertest)
        usertest_end = .;
    }
    usertest_size = usertest_end - usertest_start;

    .bss : ALIGN(4) {
        *(.bss*)
        *(COMMON)
    }

    _kernel_phys_end = .;

    . = ALIGN(4096);
    _end = .;
}
