<p align="center">
  <h1 align="center">MWOS</h1>
  <p align="center">
    <a href="README.md">中文</a> |
    <a href="README_en.md">English</a>
  </p>
</p>

A x86_64 operating system with a graphical window manager, built from scratch.

## Features

- **UEFI Boot**: Boots via UEFI firmware with OVMF
- **High-Half Kernel**: Runs in high half virtual memory
- **Window Manager**: 
  - Draggable, resizable windows
  - Minimize, maximize, and close buttons
  - Taskbar with window buttons
  - Dirty rectangle rendering for efficient redraws
- **Terminal**: Built-in terminal with ANSI color support and scrollback buffer
- **Shell**: Command-line interface with input processing
- **Filesystem**: FAT32 driver with caching support
- **Input**: PS/2 keyboard and mouse drivers
- **Graphics**: Framebuffer-based rendering with double buffering
- **Font Rendering**: TTF font support with UTF-8 text rendering and caching
- **Boot Splash**: Loading screen with progress bar and log display
- **Memory Management**: Physical and virtual memory management with paging
- **Multitasking**: Cooperative scheduler with thread support
- **PCI**: PCI bus scanning and device enumeration

## Project Structure

```
MWOS/
├── boot/              # UEFI bootloader
├── kernel/            # Kernel source code
│   ├── drivers/       # Hardware drivers (disk, PCI, PS/2)
│   │   └── fs/        # Filesystem drivers (FAT32, cache)
│   └── ui/            # UI components (microui)
├── include/           # Header files
├── assets/            # Assets (fonts, rootfs)
│   └── rootfs/        # Filesystem contents
├── programs/          # User-space programs
├── tools/             # Build tools
└── docs/              # Documentation
```

## Building

### Prerequisites

- `clang` (LLVM toolchain)
- `lld` (LLVM linker)
- `nasm` (assembler)
- `mtools` (disk image creation)
- `QEMU` (for testing)
- `OVMF.fd` (UEFI firmware for QEMU)

### Build Commands

```bash
# Build everything
make all

# Build kernel only
make kernel

# Build UEFI bootloader
make uefi

# Create disk image
make mkdisk

# Run in QEMU
make run

# Debug with GDB
make debug
```

## Running

```bash
# Run with QEMU and serial output
make run

# Run with QEMU in debug mode (GDB server on port 1234)
make debug
```

## Kernel Initialization Sequence

1. GDT initialization
2. IDT initialization
3. PIC remapping (IRQs at 0x20-0x2F)
4. PIT timer (1000 Hz)
5. PCI bus scanning
6. Disk controller initialization
7. FAT32 filesystem mounting
8. Font loading (TTF)
9. PS/2 keyboard and mouse initialization
10. Shell initialization
11. Window manager initialization
12. Terminal window creation
13. Scheduler start

## Architecture

- **Target**: x86_64-pc-win32-coff (kernel), x86_64-linux-gnu (freestanding)
- **Memory Model**: High-half kernel at 0xFFFFFFFF80000000
- **Page Size**: 4KB
- **Framebuffer**: Linear framebuffer via UEFI GOP

## License

This program is licensed under the GNU General Public License, version 2.
