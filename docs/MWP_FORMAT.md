# MWOS 可执行文件格式 (MWP) 使用文档

## 概述

**MWP (MWOS Program Format)** 是 MWOS 操作系统的自定义可执行文件格式。

- **格式名称**: MWOS Program Format
- **文件后缀**: `.mwp`
- **设计目标**: 简(fan)单(ren)、轻(fu)量(za)、快速加载

## 构建流程

```
用户 C 源码 (.c)
        │
        ▼
   clang (编译为 .o)
        │
        ▼
   ld.lld + mwp.ld (链接器脚本)
        │
        ▼
   mwp_tool (格式转换工具)
        │
        ▼
   MWP 程序 (.mwp)
```

## 快速开始

### 1. 编写程序

```c
// programs/hello.c
extern void kprintf(const char* fmt, ...);

int main(int argc, char** argv) {
    kprintf("Hello from MWP program!\n");
    return 0;
}
```

### 2. 编译程序

```bash
# 编译为目标文件
clang -target x86_64-linux-gnu -ffreestanding -fno-builtin \
      -fno-stack-protector -mno-red-zone -Wall -O2 \
      -mgeneral-regs-only -c programs/hello.c -o hello.o

# 链接为 ELF
ld.lld -T kernel/mwp.ld hello.o -o hello.elf

# 转换为 MWP 格式
tools/mwp_tool/mwp_tool hello.elf -o hello.mwp
```

### 3. 运行程序

在 MWOS Shell 中:
```
MWOS> run hello.mwp
Hello from MWP program!
```

## 文件格式规范

### 文件布局

```
+===============================+
|         MWP Header            |  64 字节 (固定)
+===============================+
|       Section Header Table    |  每个 32 字节 × N 个
+===============================+
|        Symbol Table           |  每个 24 字节 × M 个
+===============================+
|      String Table             |  变长
+===============================+
|         Section Data          |  各段实际内容
+===============================+
```

### MWP Header (64 字节)

```c
typedef struct {
    uint32_t magic;              // 0x4D575000 ('MWP\x00')
    uint16_t version;            // 格式版本号 (当前为 1)
    uint16_t type;               // 0x01=可执行, 0x02=可重定位
    uint16_t machine;            // 0x01=x86_64
    uint16_t section_count;      // 段数量
    uint16_t symbol_count;       // 符号数量
    uint32_t entry_point;        // 入口点偏移
    uint32_t string_table_offset;// 字符串表偏移
    uint32_t string_table_size;  // 字符串表大小
    uint32_t flags;              // 标志位
    uint64_t reserved;           // 保留字段
} mwp_header_t;
```

### 段类型

| 类型值 | 名称 | 说明 |
|--------|------|------|
| 0 | 未使用 | - |
| 1 | .text | 代码段 |
| 2 | .data | 已初始化数据 |
| 3 | .rodata | 只读数据 |
| 4 | .bss | 未初始化数据 |
| 5 | .symtab | 符号表 |

### 段标志

| 标志值 | 说明 |
|--------|------|
| 0x1 | 可读 |
| 0x2 | 可写 |
| 0x4 | 可执行 |

## 内核 API

MWP 程序可通过 `extern` 声明调用以下内核函数:

### 基础 I/O

```c
void kprintf(const char* fmt, ...);
void serial_puts(const char* str);
```

### 内存管理

```c
void* kmalloc(uint32_t size);
void kfree(void* ptr);
```

### 文件系统

```c
int fat32_open(const char* path, int mode);
int fat32_read(int fd, void* buf, int size);
int fat32_write(int fd, const void* buf, int size);
void fat32_close(int fd);
```

### 图形系统

```c
void draw_pixel(int x, int y, uint32_t color);
void draw_string(const char* str, int x, int y, uint32_t color);
```

### 线程

```c
void* thread_create(void (*entry)(void*), void* arg, int priority);
```

## 使用 Makefile

在项目根目录执行:

```bash
# 编译所有示例程序
make programs

# 编译单个程序
make programs/hello.mwp
```

## 示例程序

### Hello World

```c
extern void kprintf(const char* fmt, ...);

int main(int argc, char** argv) {
    kprintf("Hello from MWP program!\n");
    return 0;
}
```

### 图形绘制

```c
extern void draw_pixel(int x, int y, int color);
extern void kprintf(const char* fmt, ...);

int main(int argc, char** argv) {
    int i;
    for (i = 0; i < 100; i++) {
        draw_pixel(i, i, 0xFF0000);
    }
    kprintf("Drawing done!\n");
    return 0;
}
```

## 注意事项

1. 程序入口函数必须是 `main(int argc, char** argv)`
2. 不支持标准 C 库，需使用内核提供的 API
3. 程序运行在内核态，注意内存安全
4. 使用 `-ffreestanding` 和 `-fno-builtin` 编译

虽然不好用但是只能这么用（bushi
