<p align="center">
  <h1 align="center">MWOS</h1>
  <p align="center">
    <a href="README.md">中文</a> |
    <a href="README_en.md">English</a>
  </p>
</p>

一个从零开始构建的 x86_64 操作系统，带有图形窗口管理器。

## 功能特性

- **UEFI 启动**：通过 OVMF UEFI 固件启动
- **高半区内核**：运行在高半区虚拟内存
- **窗口管理器**：
  - 可拖拽、可调整大小的窗口
  - 最小化、最大化和关闭按钮
  - 带窗口按钮的任务栏
  - 脏矩形渲染，高效重绘
- **终端**：内置终端，支持 ANSI 颜色和滚动缓冲区
- **Shell**：命令行界面，支持输入处理
- **文件系统**：FAT32 驱动，带缓存支持
- **输入设备**：PS/2 键盘和鼠标驱动
- **图形系统**：基于帧缓冲的渲染，支持双缓冲
- **字体渲染**：TTF 字体支持，UTF-8 文本渲染和缓存
- **启动画面**：带进度条和日志显示的加载界面
- **内存管理**：物理和虚拟内存管理，支持分页
- **多任务**：协作式调度器，支持线程
- **PCI**：PCI 总线扫描和设备枚举

## 项目结构

```
MWOS/
├── boot/              # UEFI 引导加载程序
├── kernel/            # 内核源代码
│   ├── drivers/       # 硬件驱动（磁盘、PCI、PS/2）
│   │   └── fs/        # 文件系统驱动（FAT32、缓存）
│   └── ui/            # UI 组件（microui）
├── include/           # 头文件
├── assets/            # 资源文件（字体、rootfs）
│   └── rootfs/        # 文件系统内容
├── programs/          # 用户空间程序
├── tools/             # 构建工具
└── docs/              # 文档
```

## 构建

### 前置依赖

- `clang`（LLVM 工具链）
- `lld`（LLVM 链接器）
- `nasm`（汇编器）
- `mtools`（磁盘镜像创建）
- `QEMU`（用于测试）
- `OVMF.fd`（QEMU 的 UEFI 固件）

### 构建命令

```bash
# 构建所有内容
make all

# 仅构建内核
make kernel

# 构建 UEFI 引导加载程序
make uefi

# 创建磁盘镜像
make mkdisk

# 在 QEMU 中运行
make run

# 使用 GDB 调试
make debug
```

## 运行

```bash
# 在 QEMU 中运行，带串口输出
make run

# 在 QEMU 中以调试模式运行（GDB 服务器在端口 1234）
make debug
```

## 内核初始化流程

1. GDT 初始化
2. IDT 初始化
3. PIC 重映射（IRQ 位于 0x20-0x2F）
4. PIT 定时器（1000 Hz）
5. PCI 总线扫描
6. 磁盘控制器初始化
7. FAT32 文件系统挂载
8. 字体加载（TTF）
9. PS/2 键盘和鼠标初始化
10. Shell 初始化
11. 窗口管理器初始化
12. 终端窗口创建
13. 调度器启动

## 架构

- **目标平台**：x86_64-pc-win32-coff（内核），x86_64-linux-gnu（独立环境）
- **内存模型**：高半区内核位于 0xFFFFFFFF80000000
- **页大小**：4KB
- **帧缓冲**：通过 UEFI GOP 线性帧缓冲

## 许可证

本项目用于教育目的。
