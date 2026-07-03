# ============================
# Linux Kernel 风格输出控制
# ============================

ifeq ($(V),1)
  Q :=
else
  Q := @
endif

CC_MSG      = echo "  CC          $@"
AS_MSG      = echo "  AS          $@"
LD_MSG      = echo "  LD          $@"
OBJCOPY_MSG = echo "  OBJCOPY     $@"
EFI_MSG     = echo "  EFI         $@"
DISK_MSG    = echo "  DISK        $@"

# ============================
# 工具链配置
# ============================

CC = clang
CFLAGS = -target x86_64-pc-win32-coff -mno-red-zone -fno-stack-protector -fshort-wchar -Wall -Wextra -Iinclude/ \
         -Iinclude/freestnd-c-hdrs/ -Wno-unused-variable -Wno-unused-parameter -Wno-unused-but-set-variable \
         -Wno-incompatible-library-redeclaration
AS = nasm
LD = lld-link

# ============================
# 目录配置
# ============================

SRCDIR = .
BOOTDIR = $(SRCDIR)/boot
EFIDIR = $(BOOTDIR)
KERNELDIR = $(SRCDIR)/kernel
PROGRAMSDIR = $(SRCDIR)/programs
BUILDDIR ?= $(SRCDIR)/build
EFIBUILDDIR = $(BUILDDIR)/efi
KERNELBUILDDIR = $(BUILDDIR)/kernel

KERNEL_SUBDIRS := $(shell find $(KERNELDIR) -type d)

EFI_SOURCES = $(wildcard $(EFIDIR)/*.c)
KERNEL_ALL_C_SOURCES = $(shell find $(KERNELDIR) -type f -name '*.c')
KERNEL_ALL_ASM_SOURCES = $(shell find $(KERNELDIR) -type f -name '*.asm')

EFI_OBJECTS = $(patsubst $(EFIDIR)/%.c, $(EFIBUILDDIR)/%.o, $(EFI_SOURCES))

KERNEL_C_OBJECTS = $(patsubst $(KERNELDIR)/%.c, $(KERNELBUILDDIR)/%.o, $(KERNEL_ALL_C_SOURCES))
KERNEL_ASM_OBJECTS = $(patsubst $(KERNELDIR)/%.asm, $(KERNELBUILDDIR)/%.o, $(KERNEL_ALL_ASM_SOURCES))
KERNEL_OBJECTS = $(KERNEL_C_OBJECTS) $(KERNEL_ASM_OBJECTS)

# ============================
# 入口文件自动检测
# ============================

ifneq ($(wildcard $(KERNELDIR)/kmain.c),)
    ENTRY_OBJECT = $(KERNELBUILDDIR)/kmain.o
else ifneq ($(wildcard $(KERNELDIR)/kernel.c),)
    ENTRY_OBJECT = $(KERNELBUILDDIR)/kernel.o
else ifneq ($(wildcard $(KERNELDIR)/main.c),)
    ENTRY_OBJECT = $(KERNELBUILDDIR)/main.o
else
    ENTRY_OBJECT = $(firstword $(KERNEL_OBJECTS))
endif

OTHER_OBJECTS = $(filter-out $(ENTRY_OBJECT), $(KERNEL_OBJECTS))

KERNEL_INCLUDE_DIRS = $(addprefix -I, $(KERNEL_SUBDIRS))

KERNEL_CFLAGS = -target x86_64-linux-gnu -ffreestanding -fno-builtin \
                -fno-stack-protector -mno-red-zone -Wall -Wextra -O2 -g $(INCLUDE) \
                -mgeneral-regs-only -mno-sse -mno-mmx -Iinclude/ -I./include/freestnd-c-hdrs/ \
                $(KERNEL_INCLUDE_DIRS) -Wno-unused-variable -Wno-unused-parameter \
                -Wno-unused-but-set-variable -Wno-unused-function -Wno-comment \
                -Wno-pragma-pack -Wno-self-assign -Wno-incompatible-library-redeclaration \
                -Wno-sign-compare -Wno-tautological-constant-out-of-range-compare \
                -Wno-incompatible-library-redeclaration -I./include/zlib -Wno-pointer-to-int-cast

.PHONY: all clean uefi kernel disk mkdisk run debug list-sources list-dirs compiledb programs

# ============================
# 默认目标
# ============================

all: uefi kernel programs disk mkdisk compiledb

# ============================
# UEFI 构建
# ============================

uefi: $(EFIBUILDDIR)/bootx64.efi

$(EFIBUILDDIR)/bootx64.efi: $(EFI_OBJECTS)
	$(Q)mkdir -p $(EFIBUILDDIR)
	$(Q)$(LD_MSG)
	$(Q)$(LD) /subsystem:efi_application /entry:efi_main /out:$@ $^

$(EFIBUILDDIR)/%.o: $(EFIDIR)/%.c
	$(Q)mkdir -p $(EFIBUILDDIR)
	$(Q)$(CC_MSG)
	$(Q)$(CC) $(CFLAGS) -I$(EFIDIR) -c $< -o $@


# ============================
# Kernel 构建
# ============================

kernel: $(KERNELBUILDDIR)/kernel.bin

$(KERNELBUILDDIR)/kernel.bin: $(KERNELBUILDDIR)/kernel.elf
	$(Q)$(OBJCOPY_MSG)
	$(Q)llvm-objcopy -O binary $< $@

$(KERNELBUILDDIR)/kernel.elf: $(KERNEL_OBJECTS) $(KERNELDIR)/kernel.ld
	$(Q)mkdir -p $(KERNELBUILDDIR)
	$(Q)$(LD_MSG)
	$(Q)ld.lld -nostdlib -T $(KERNELDIR)/kernel.ld -o $@ $(ENTRY_OBJECT) $(OTHER_OBJECTS)

# ttf.o 需要使用浮点运算（stb_truetype），去掉 -mgeneral-regs-only -mno-sse -mno-mmx
TTF_CFLAGS = $(filter-out -mgeneral-regs-only -mno-sse -mno-mmx, $(KERNEL_CFLAGS))

$(KERNELBUILDDIR)/ttf.o: $(KERNELDIR)/ttf.c
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC_MSG)
	$(Q)$(CC) $(TTF_CFLAGS) -c $< -o $@

$(KERNELBUILDDIR)/%.o: $(KERNELDIR)/%.c
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC_MSG)
	$(Q)$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNELBUILDDIR)/%.o: $(KERNELDIR)/%.asm
	$(Q)mkdir -p $(dir $@)
	$(Q)$(AS_MSG)
	$(Q)$(AS) -f elf64 -o $@ $<

# ============================
# User Programs
# ============================

.PHONY: programs

programs:
	$(Q)$(MAKE) --no-print-directory -C $(PROGRAMSDIR) PROGRAMS_BUILDDIR=$(abspath $(BUILDDIR))

# ============================
# Kernel Image (仅内核)
# ============================

disk: $(BUILDDIR)/kernel.img

$(BUILDDIR)/kernel.img: uefi kernel
	$(Q)mkdir -p $(BUILDDIR)/EFI/BOOT
	$(Q)dd if=/dev/zero of=$@ bs=1M count=64 2>/dev/null
	$(Q)$(DISK_MSG)
	$(Q)mkfs.fat -F 16 $@ >/dev/null 2>&1
	$(Q)mmd -i $@ ::EFI
	$(Q)mmd -i $@ ::EFI/BOOT
	$(Q)mcopy -i $@ $(EFIBUILDDIR)/bootx64.efi ::EFI/BOOT/bootx64.efi
	$(Q)mcopy -i $@ $(KERNELBUILDDIR)/kernel.bin ::kernel.bin

# ============================
# Disk Image (完整磁盘，含 assets/rootfs 内容)
# ============================

mkdisk: $(BUILDDIR)/disk.img

$(BUILDDIR)/disk.img: uefi kernel programs
	$(Q)echo "  MKDISK      $(BUILDDIR)/disk.img (1GB, ESP+FAT32 + EXT2)"
	$(Q)mkdir -p $(BUILDDIR)
	$(Q)dd if=/dev/zero of=$@ bs=1M count=1024 2>/dev/null
	@# 创建 MBR 分区表：P1=FAT32(ESP) 64MB, P2=EXT2 剩余空间
	$(Q)echo "2048,131072,0x0C,*"  > $(BUILDDIR)/partitions.sfdisk
	$(Q)echo "133120,,0x83"       >> $(BUILDDIR)/partitions.sfdisk
	$(Q)sfdisk $@ < $(BUILDDIR)/partitions.sfdisk >/dev/null 2>&1
	$(Q)rm -f $(BUILDDIR)/partitions.sfdisk
	@# 创建 FAT32 ESP 分区并写入 bootloader
	$(Q)dd if=/dev/zero of=$(BUILDDIR)/esp.img bs=512 count=131072 2>/dev/null
	$(Q)mkfs.fat -F 32 $(BUILDDIR)/esp.img >/dev/null 2>&1
	$(Q)mmd -i $(BUILDDIR)/esp.img ::EFI
	$(Q)mmd -i $(BUILDDIR)/esp.img ::EFI/BOOT
	$(Q)mcopy -i $(BUILDDIR)/esp.img $(EFIBUILDDIR)/bootx64.efi ::EFI/BOOT/bootx64.efi
	$(Q)mcopy -i $(BUILDDIR)/esp.img $(KERNELBUILDDIR)/kernel.bin ::kernel.bin
	$(Q)dd if=$(BUILDDIR)/esp.img of=$@ bs=512 seek=2048 conv=notrunc 2>/dev/null
	$(Q)rm -f $(BUILDDIR)/esp.img
	@# 创建 EXT2 root 分区（预填充 kernel.bin + rootfs）
	$(Q)dd if=/dev/zero of=$(BUILDDIR)/root.img bs=512 count=1964032 2>/dev/null
	$(Q)mkdir -p $(BUILDDIR)/rootfs_staging
	$(Q)cp $(KERNELBUILDDIR)/kernel.bin $(BUILDDIR)/rootfs_staging/kernel.bin
	$(Q)cp -r $(BUILDDIR)/programs/*.elf $(BUILDDIR)/rootfs_staging/ 2>/dev/null; ls $(BUILDDIR)/rootfs_staging/ 2>/dev/null || true
	$(Q)if [ -d assets/rootfs ]; then \
		cp -r assets/rootfs/* $(BUILDDIR)/rootfs_staging/ 2>/dev/null || true; \
	fi
	$(Q)mkfs.ext2 -F -d $(BUILDDIR)/rootfs_staging $(BUILDDIR)/root.img >/dev/null 2>&1
	$(Q)dd if=$(BUILDDIR)/root.img of=$@ bs=512 seek=133120 conv=notrunc 2>/dev/null
	$(Q)rm -rf $(BUILDDIR)/root.img $(BUILDDIR)/rootfs_staging
	$(Q)echo "  DONE        disk.img created (ESP @ LBA 2048, EXT2 @ LBA 133120)"

# ============================
# Run & Debug
# ============================

run: mkdisk
	qemu-system-x86_64 -bios OVMF.fd -drive file=$(BUILDDIR)/disk.img,format=raw -serial stdio -m 1G

# 高半区偏移量 = 0xFFFFFFFF80000000 (KERNEL_VIRT_BASE) - 0x100000 (kernel phys load addr)
HIGH_HALF_OFFSET := 0xFFFFFFFF7FF00000

debug: mkdisk
	@echo "==> QEMU with GDB server on localhost:1234"
	@echo "==> Connect GDB: gdb -x scripts/gdb_highhalf.gdb"
	qemu-system-x86_64 -bios OVMF.fd -drive file=$(BUILDDIR)/disk.img,format=raw -serial stdio -s -S -m 1G

q35: mkdisk
	@echo "==> QEMU (Q35) with GDB server on localhost:1234"
	@echo "==> Connect GDB: gdb -x scripts/gdb_highhalf.gdb"
	@echo "     (symbols auto-loaded with high-half offset)"
	qemu-system-x86_64 \
		-M q35 \
		-bios OVMF.fd \
		-drive file=$(BUILDDIR)/disk.img,format=raw,if=ide-hd \
		-serial stdio \
		-s -S \
		-m 1G \
		-vga std \
		-device e1000,netdev=net0 \
		-netdev user,id=net0 \
		-device AC97 \
		-usb -device usb-tablet

gdb-q35: mkdisk
	@echo "==> Starting QEMU (Q35) in background with GDB server..."
	@qemu-system-x86_64 \
		-M q35 \
		-bios OVMF.fd \
		-drive file=$(BUILDDIR)/disk.img,format=raw \
		-serial stdio \
		-s -S \
		-m 1G \
		-vga std \
		-device e1000,netdev=net0 \
		-netdev user,id=net0 \
		-device AC97 \
		-usb -device usb-tablet &
	@sleep 2
	@echo "==> Launching GDB (high-half symbols)..."
	@gdb -x scripts/gdb_highhalf.gdb

# ============================
# Compile Commands (clangd)
# ============================

compiledb:
	$(Q)if command -v bear >/dev/null 2>&1; then \
		echo "  BEAR        Generating compile_commands.json"; \
		bear -- $(MAKE) --no-print-directory _compile_for_bear; \
	fi

_compile_for_bear:
	$(Q)$(MAKE) --no-print-directory clean
	$(Q)$(MAKE) --no-print-directory uefi kernel

# ============================
# Clean
# ============================

clean:
	$(Q)$(MAKE) --no-print-directory -C $(PROGRAMSDIR) clean PROGRAMS_BUILDDIR=$(abspath $(BUILDDIR)) 2>/dev/null || true
	rm -rf $(BUILDDIR)

# ============================
# Info
# ============================

list-sources:
	@echo "Entry Object: $(ENTRY_OBJECT)"
	@echo "Kernel C Sources: $(words $(KERNEL_ALL_C_SOURCES)) files"
	@echo "Kernel ASM Sources: $(words $(KERNEL_ALL_ASM_SOURCES)) files"
	@echo "Total Objects: $(words $(KERNEL_OBJECTS))"
	@echo ""
	@echo "Build directory will contain:"
	@for obj in $(sort $(KERNEL_OBJECTS)); do \
		echo "  $$obj"; \
	done

list-dirs:
	@echo "Kernel subdirectories:"
	@for dir in $(sort $(KERNEL_SUBDIRS)); do \
		echo "  $$dir"; \
	done


