# ============================================================
# GDB init script for MWOS high-half kernel debugging
#
# Usage:
#   1. Start QEMU:  make q35         (waits for GDB)
#   2. Connect:     gdb -x scripts/gdb_highhalf.gdb
#
# Or use the all-in-one target:  make gdb-q35
#
# How it works:
#   Kernel ELF symbols are at low addresses (0x100000 range),
#   but the kernel jumps to high-half (0xFFFFFFFF80000000+)
#   before running kmain.
#
#   We use 'symbol-file -o <offset>' to shift ALL symbols by:
#     KERNEL_VIRT_BASE - _kernel_phys_start
#   = 0xFFFFFFFF80000000 - 0x100000
#   = 0xFFFFFFFF7FF00000
#
#   So 'break kmain' sets a breakpoint at 0xFFFFFFFF80000180,
#   which is where kmain actually executes.
# ============================================================

target remote localhost:1234
symbol-file -o 0xFFFFFFFF7FF00000 build/kernel/kernel.elf

echo \n
echo === MWOS High-Half Debug Session ===\n
echo   Symbols loaded at KERNEL_VIRT_BASE + 0x100000\n
echo   Try: break kmain\n
echo   Try: continue\n
echo =====================================\n
