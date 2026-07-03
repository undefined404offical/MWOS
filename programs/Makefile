# MWOS 用户程序构建规则
#
# 用法: make [all|clean] PROGRAMS_BUILDDIR=path
#
# 输出: $(PROGRAMS_BUILDDIR)/programs/ 目录下的 .elf 文件

PROGRAMS_BUILDDIR ?= ../build
CC = clang
CFLAGS = -target x86_64-linux-gnu -ffreestanding -fno-builtin \
         -fno-stack-protector -mno-red-zone -Wall -Wextra -O2 -g \
         -I. -I../include -Wno-unused-parameter -Wno-unused-variable
LD = ld.lld
LDFLAGS = -nostdlib -T user.ld

PROGRAMSDIR = .
BUILDIR = $(PROGRAMS_BUILDDIR)/programs
ELFS = $(BUILDIR)/hello.elf

.PHONY: all clean

all: $(ELFS)

$(BUILDIR):
	mkdir -p $(BUILDIR)

$(BUILDIR)/hello.o: $(PROGRAMSDIR)/hello.c | $(BUILDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDIR)/hello.elf: $(BUILDIR)/hello.o
	$(LD) $(LDFLAGS) -o $@ $^
	@echo "  PROGRAM    $@"

clean:
	rm -rf $(BUILDIR)
