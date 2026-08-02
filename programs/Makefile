# ============================
# User Programs Build
# ============================

CC = clang
AS = nasm
LD = lld-link

# Output directory (from main Makefile)
PROGRAMS_OUTPUT_DIR ?= $(PROGRAMS_BUILDDIR)/programs

# Program sources (assembly)
ASM_SRCS := $(wildcard *.asm)

# Output ELF targets
ASM_ELF_TARGETS := $(patsubst %.asm, $(PROGRAMS_OUTPUT_DIR)/%.elf, $(ASM_SRCS))

# syscall_nr.h 路径
SYSCALL_NR_H := $(abspath $(dir $(firstword $(MAKEFILE_LIST)))/../include/syscall_nr.h)

# musl 路径
MUSL_DIR := $(abspath $(dir $(firstword $(MAKEFILE_LIST)))/musl)
MUSL_LIB := $(MUSL_DIR)/lib
MUSL_INC := $(MUSL_DIR)/include $(MUSL_DIR)/obj/include $(MUSL_DIR)/arch/x86_64 $(MUSL_DIR)/arch/generic

# C 程序源文件
C_SRCS := $(wildcard *.c)
C_ELF_TARGETS := $(patsubst %.c, $(PROGRAMS_OUTPUT_DIR)/%.elf, $(C_SRCS))

.PHONY: all clean

all: $(PROGRAMS_OUTPUT_DIR) $(ASM_ELF_TARGETS) $(C_ELF_TARGETS)

$(PROGRAMS_OUTPUT_DIR):
	mkdir -p $(PROGRAMS_OUTPUT_DIR)

# 汇编程序构建规则
$(PROGRAMS_OUTPUT_DIR)/%.elf: %.asm $(SYSCALL_NR_H)
	$(Q)echo "  ASM         $@"
	$(Q)$(AS) -f elf64 -o $(PROGRAMS_OUTPUT_DIR)/$*.o $<
	$(Q)echo "  LD          $@"
	$(Q)ld.lld -nostdlib -o $@ \
		-Ttext=0x400000 \
		--entry=_start \
		$(PROGRAMS_OUTPUT_DIR)/$*.o

# C 程序构建规则 (musl 链接)
$(PROGRAMS_OUTPUT_DIR)/hello.elf: hello.c $(MUSL_LIB)/libc.a $(MUSL_LIB)/crt1.o
	$(Q)echo "  CC          programs/$<"
	$(Q)$(CC) -target x86_64-linux-gnu -ffreestanding -nostdinc \
		$(addprefix -I, $(MUSL_INC)) \
		-I$(abspath $(dir $(firstword $(MAKEFILE_LIST)))/../include) \
		-c -o $(PROGRAMS_OUTPUT_DIR)/hello.o $<
	$(Q)echo "  LD          $@"
	$(Q)ld.lld -nostdlib -o $@ \
		-Ttext=0x400000 \
		--entry=_start \
		$(MUSL_LIB)/crt1.o \
		$(PROGRAMS_OUTPUT_DIR)/hello.o \
		-L$(MUSL_LIB) -lc

clean:
	rm -rf $(PROGRAMS_OUTPUT_DIR) 2>/dev/null || true