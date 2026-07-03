symbol-file /home/wsc/文档/MWOS/build/kernel/kernel.elf
target remote :1234

# Set breakpoints at key locations
# kmain is linked at 0x100000 + offset
# Use the function name - GDB resolves it

# Break at the start of the ring 3 test code
break kmain

echo === Waiting for kernel to boot... ===\n
echo Type 'continue' to start\n
