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

.PHONY: all clean uefi kernel disk mkdisk run debug list-sources list-dirs mwp-tool programs compiledb

# ============================
# 默认目标
# ============================

all: uefi kernel disk mkdisk compiledb

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

$(BUILDDIR)/disk.img: uefi kernel
	$(Q)echo "  MKDISK      $(BUILDDIR)/disk.img (1GB)"
	$(Q)mkdir -p $(BUILDDIR)/EFI/BOOT
	$(Q)dd if=/dev/zero of=$(BUILDDIR)/disk.img bs=1M count=1024 2>/dev/null
	$(Q)mkfs.fat -F 32 $(BUILDDIR)/disk.img >/dev/null 2>&1
	$(Q)mmd -i $(BUILDDIR)/disk.img ::EFI
	$(Q)mmd -i $(BUILDDIR)/disk.img ::EFI/BOOT
	$(Q)mcopy -i $(BUILDDIR)/disk.img $(EFIBUILDDIR)/bootx64.efi ::EFI/BOOT/bootx64.efi
	$(Q)mcopy -i $(BUILDDIR)/disk.img $(KERNELBUILDDIR)/kernel.bin ::kernel.bin
	$(Q)if [ -d assets/rootfs ]; then \
		echo "  PACKING     assets/rootfs -> disk.img"; \
		(cd assets/rootfs && find . -type d | sed 's|^\./||' | grep -v '^\.$$' | sort | while read dir; do \
			mmd -i ../../$(BUILDDIR)/disk.img ::"$$dir" 2>/dev/null || true; \
		done && find . -type f | sed 's|^\./||' | while read file; do \
			mcopy -i ../../$(BUILDDIR)/disk.img "$$file" ::"$$file" 2>/dev/null || echo "  WARN: failed to copy $$file"; \
		done); \
	fi
	$(Q)echo "  DONE        disk.img created"

# ============================
# Run & Debug
# ============================

run: mkdisk
	qemu-system-x86_64 -bios OVMF.fd -drive file=$(BUILDDIR)/disk.img,format=raw -serial stdio -m 1G

debug: mkdisk
	qemu-system-x86_64 -bios OVMF.fd -drive file=$(BUILDDIR)/disk.img,format=raw -serial stdio -s -S -m 1G

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

# ============================
# MWP Program Build
# ============================

MWP_TOOL_DIR = tools/mwp_tool
MWP_TOOL = $(MWP_TOOL_DIR)/mwp_tool

$(MWP_TOOL): $(MWP_TOOL_DIR)/mwp_tool.c
	$(Q)echo "  BUILD       mwp_tool"
	$(Q)gcc -Iinclude/ -o $@ $< -O2

MWP_CFLAGS = -target x86_64-linux-gnu -ffreestanding -fno-builtin \
             -fno-stack-protector -mno-red-zone -Wall -Wextra -O2 \
             -mgeneral-regs-only -Iinclude/ -Iinclude/freestnd-c-hdrs/ \
             -Wno-unused-variable -Wno-unused-parameter

programs/%.mwp: programs/%.c $(MWP_TOOL)
	$(Q)mkdir -p programs
	$(Q)echo "  MWPCC       $@"
	$(Q)$(CC) $(MWP_CFLAGS) -c $< -o programs/$*.o
	$(Q)ld.lld --unresolved-symbols=ignore-all -T kernel/mwp.ld programs/$*.o -o programs/$*.elf
	$(Q)$(MWP_TOOL) programs/$*.elf -o $@
	$(Q)rm -f programs/$*.o programs/$*.elf

PROGRAMS = programs/hello.mwp

programs: $(MWP_TOOL) $(PROGRAMS)
	@echo "MWP programs built successfully"
