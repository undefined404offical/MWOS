#include "bootloader.h"
#include "serial.h"

// #define COMPATIBLE_MODE
// 兼容模式，优先使用1920x1080
// 除非显示器或显卡不支持，否则不建议使用

// memset实现
void* memset(void* s, int c, unsigned long n) {
    unsigned char* p = s;
    while (n--) {
        *p++ = (unsigned char)c;
    }
    return s;
}

#ifndef EFI_PAGE_SIZE
#define EFI_PAGE_SIZE 4096
#endif
#define KERNEL_STACK_SIZE (64 * 1024)
#define PAGES_FOR_STACK (KERNEL_STACK_SIZE / 4096)

EFI_HANDLE gImageHandle = NULL;
static void* gKernelBase = NULL;
static UINTN gKernelSize = 0;

void set_image_handle(EFI_HANDLE handle) { gImageHandle = handle; }

void print_string(CHAR16* str) { gST->ConOut->OutputString(gST->ConOut, str); }

void print_error(CHAR16* message, EFI_STATUS status) {
    print_string(message);
    print_string(L": ");

    CHAR16 buffer[32];
    for (int i = 0; i < 16; i++) {
        UINT8 nibble = (status >> (60 - i * 4)) & 0xF;
        buffer[i] = (nibble < 10) ? (L'0' + nibble) : (L'A' + nibble - 10);
    }
    buffer[16] = L'\r';
    buffer[17] = L'\n';
    buffer[18] = L'\0';

    print_string(buffer);
}

UINTN wsprintf(CHAR16* buffer, const CHAR16* format, ...) {
    UINTN count = 0;
    const CHAR16* p = format;

    UINTN* arg_ptr = (UINTN*)(&format + 1);

    while (*p) {
        if (*p == L'%') {
            p++;
            switch (*p) {
            case L'd': {
                UINT32 value = (UINT32)*arg_ptr++;
                UINT32 temp = value;
                UINT32 digits = 0;

                do {
                    digits++;
                    temp /= 10;
                } while (temp > 0);

                // 处理值为0的情况
                if (digits == 0)
                    digits = 1;

                temp = value;
                for (UINT32 i = 0; i < digits; i++) {
                    buffer[count + digits - 1 - i] = L'0' + (temp % 10);
                    temp /= 10;
                }
                count += digits;
                break;
            }
            case L'x': {
                UINT32 value = (UINT32)*arg_ptr++;
                UINT32 temp = value;
                UINT32 digits = 0;

                do {
                    digits++;
                    temp >>= 4;
                } while (temp > 0);

                // 处理值为0的情况
                if (digits == 0)
                    digits = 1;

                temp = value;
                for (UINT32 i = 0; i < digits; i++) {
                    UINT8 nibble = (temp >> ((digits - 1 - i) * 4)) & 0xF;
                    buffer[count + i] =
                        (nibble < 10) ? (L'0' + nibble) : (L'A' + nibble - 10);
                }
                count += digits;
                break;
            }
            default:
                buffer[count++] = L'%';
                buffer[count++] = *p;
                break;
            }
            p++;
        } else {
            buffer[count++] = *p++;
        }
    }

    buffer[count] = L'\0';
    return count;
}

EFI_STATUS open_root_directory(EFI_FILE_PROTOCOL** Root) {
    EFI_STATUS status;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* FileSystem;
    EFI_GUID FileSystemGuid = {
        0x0964e5b22,
        0x6459,
        0x11d2,
        {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};

    status = gST->BootServices->LocateProtocol(&FileSystemGuid, NULL,
                                               (VOID**)&FileSystem);
    if (EFI_ERROR(status)) {
        print_string(L"[X] Locate file system protocol\r\n");
    }

    print_string(L"[OK] Found file system protocol\r\n");

    status = FileSystem->OpenVolume(FileSystem, Root);
    if (EFI_ERROR(status)) {
        print_string(L"[X] Open root directory\r\n");
    }

    print_string(L"[OK] Open root directory\r\n");
    return EFI_SUCCESS;
}

EFI_STATUS load_file(EFI_FILE_PROTOCOL* Root, CHAR16* FileName, void** Buffer,
                     UINTN* FileSize) {
    EFI_STATUS status;
    EFI_FILE_PROTOCOL* File;
    EFI_FILE_INFO* FileInfo;
    EFI_GUID FileInfoGuid = EFI_FILE_INFO_ID;
    UINTN InfoSize = 0;

    status = Root->Open(Root, &File, FileName, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) {
        print_string(L"[X] Open file: ");
        print_string(FileName);
        print_string(L"\r\n");
    } else {
        print_string(L"[OK] Open file: ");
        print_string(FileName);
        print_string(L"\r\n");
    }

    status = File->GetInfo(File, &FileInfoGuid, &InfoSize, NULL);
    if (status != EFI_BUFFER_TOO_SMALL) {
        print_string(L"[X] Get file info size\r\n");
        File->Close(File);
    } else {
        print_string(L"[OK] Get file info size\r\n");
    }
    status = gST->BootServices->AllocatePool(EfiLoaderData, InfoSize,
                                             (VOID**)&FileInfo);
    if (EFI_ERROR(status)) {
        print_string(L"[X] Allocate memory for file info\r\n");
        File->Close(File);
    } else {
        print_string(L"[OK] Allocate memory for file info\r\n");
    }

    status = File->GetInfo(File, &FileInfoGuid, &InfoSize, FileInfo);
    if (EFI_ERROR(status)) {
        print_string(L"[X] Get file info\r\n");
        gST->BootServices->FreePool(FileInfo);
        File->Close(File);
    } else {
        print_string(L"[OK] Get file info\r\n");
    }

    *FileSize = FileInfo->FileSize;

    status = gST->BootServices->AllocatePool(EfiLoaderData, *FileSize, Buffer);
    if (EFI_ERROR(status)) {
        print_string(L"[X] Allocate memory for file\r\n");
        gST->BootServices->FreePool(FileInfo);
        File->Close(File);
    } else {
        print_string(L"[OK] Allocate memory for file\r\n");
    }

    UINTN ReadSize = *FileSize;
    status = File->Read(File, &ReadSize, *Buffer);
    if (EFI_ERROR(status)) {
        print_string(L"[X] Read file\r\n");
        gST->BootServices->FreePool(*Buffer);
        gST->BootServices->FreePool(FileInfo);
        File->Close(File);
    }

    if (ReadSize != *FileSize) {
        print_string(L"[!] Read size doesn't match file size\r\n");
    }

    print_string(L"[OK] Read file\r\n");

    gST->BootServices->FreePool(FileInfo);
    File->Close(File);

    return EFI_SUCCESS;
}

// 加载内核到1MB地址
EFI_STATUS load_kernel(void) {
    EFI_STATUS status;
    EFI_FILE_PROTOCOL* Root;

    status = open_root_directory(&Root);
    if (EFI_ERROR(status)) {
        print_string(L"[X] Open root directory\r\n");
        return status;
    }

    print_string(L"[OK] Open root directory\r\n");

    void* temp_buffer = NULL;
    UINTN file_size = 0;

    status = load_file(Root, L"kernel.bin", &temp_buffer, &file_size);
    if (EFI_ERROR(status)) {
        print_string(L"[X] Load kernel file\r\n");
        return status;
    }

    print_string(L"[OK] Load kernel file to temp buffer\r\n");

    UINTN pages = (file_size + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE;

    // 定义1MB地址 (0x100000)
    EFI_PHYSICAL_ADDRESS kernel_address = 0x100000;

    // 在1MB地址处分配内存
    status = gST->BootServices->AllocatePages(AllocateAddress, EfiLoaderData,
                                              pages, &kernel_address);
    if (EFI_ERROR(status)) {
        print_string(L"[X] Allocate memory at 1MB\r\n");
        print_error(L"AllocatePages error", status);

        kernel_address = 0x100000;
        status = gST->BootServices->AllocatePages(
            AllocateAnyPages, EfiLoaderData, pages, &kernel_address);
        if (EFI_ERROR(status)) {
            print_string(L"[X] Failed to allocate memory for kernel\r\n");
            gST->BootServices->FreePool(temp_buffer);
            return status;
        }
        print_string(L"[OK] Allocated memory at dynamic address\r\n");
    } else {
        print_string(L"[OK] Allocated memory at 1MB (0x100000)\r\n");
    }

    // 将内核复制到1MB地址
    gKernelBase = (void*)(UINT64)kernel_address;
    gKernelSize = file_size;

    gST->BootServices->CopyMem(gKernelBase, temp_buffer, file_size);

    gST->BootServices->FreePool(temp_buffer);

    print_string(L"[->] Kernel loaded at: ");
    CHAR16 addr_msg[64];
    wsprintf(addr_msg, L"0x%x%08x\r\n", (UINT32)((UINT64)gKernelBase >> 32),
             (UINT32)((UINT64)gKernelBase));
    print_string(addr_msg);

    return EFI_SUCCESS;
}

EFI_STATUS boot_kernel(void) {
    if (gKernelBase == NULL) {
        print_string(L"[X] Kernel not loaded\r\n");
        return EFI_NOT_READY;
    }

    print_string(L"[->] Kernel base address: ");
    CHAR16 kernel_addr_msg[64];
    wsprintf(kernel_addr_msg, L"0x%x%08x", (UINT32)((UINT64)gKernelBase >> 32),
             (UINT32)((UINT64)gKernelBase));
    print_string(kernel_addr_msg);
    print_string(L"\r\n");

    CHAR16 kernel_size_msg[64];
    wsprintf(kernel_size_msg, L"[->] Kernel size: %ld bytes\r\n", gKernelSize);
    print_string(kernel_size_msg);

    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = NULL;
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_STATUS status =
        gST->BootServices->LocateProtocol(&gop_guid, NULL, (VOID**)&gop);

    // 内核参数结构（一定要与内核中的 boot_params_t 匹配!）
#pragma pack(push, 1)
    typedef struct {
        uint64_t framebuffer_addr;
        uint32_t framebuffer_width;
        uint32_t framebuffer_height;
        uint32_t framebuffer_pitch;
        uint32_t framebuffer_bpp;
        uint64_t framebuffer_size;

        uint64_t memory_map_addr;
        uint64_t memory_map_size;
        uint64_t descriptor_size;
    } boot_params_t;
#pragma pack(pop)

    // 在 ExitBootServices 之前分配内存存储参数
    boot_params_t* params = NULL;
    gST->BootServices->AllocatePool(EfiLoaderData, sizeof(boot_params_t),
                                    (VOID**)&params);
    if (params == NULL) {
        print_error(L"[X] Failed to allocate memory for params",
                    EFI_OUT_OF_RESOURCES);
        return EFI_OUT_OF_RESOURCES;
    }
    memset(params, 0, sizeof(boot_params_t));

    if (!EFI_ERROR(status) && gop && gop->Mode) {
        print_string(L"[OK] Graphics output protocol found\r\n");

        // 查询所有可用的图形模式（只考虑 32bpp 模式）
        UINTN max_mode = gop->Mode->MaxMode;
        UINTN best_mode = gop->Mode->Mode;
        UINTN best_width = 0, best_height = 0;
        BOOLEAN found_target = 0;

        print_string(L"[->] Getting graphics modes...\r\n");

        for (UINTN i = 0; i < max_mode; i++) {
            EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* mode_info = NULL;
            UINTN size_of_info = 0;

            EFI_STATUS st = gop->QueryMode(gop, i, &size_of_info, &mode_info);
            if (EFI_ERROR(st) || !mode_info) {
                continue;
            }

            // 只接受 32bpp 模式（BGRX 或 RGBX）
            BOOLEAN is_32bpp = (mode_info->PixelFormat ==
                                PixelBlueGreenRedReserved8BitPerColor) ||
                               (mode_info->PixelFormat ==
                                PixelRedGreenBlueReserved8BitPerColor);

            if (!is_32bpp) {
                // 跳过所有非 32bpp 模式
                gST->BootServices->FreePool(mode_info);
                continue;
            }

            print_string(L"[->] Mode: ");

            CHAR16 mode_num[16];
            UINT32 temp = (UINT32)i;
            UINT32 digits = 0;
            do {
                digits++;
                temp /= 10;
            } while (temp > 0);
            temp = (UINT32)i;
            for (UINT32 j = 0; j < digits; j++) {
                mode_num[digits - 1 - j] = L'0' + (temp % 10);
                temp /= 10;
            }
            mode_num[digits] = L'\0';
            print_string(mode_num);

            print_string(L" Resolution: ");

            temp = mode_info->HorizontalResolution;
            digits = 0;
            do {
                digits++;
                temp /= 10;
            } while (temp > 0);
            temp = mode_info->HorizontalResolution;
            for (UINT32 j = 0; j < digits; j++) {
                mode_num[digits - 1 - j] = L'0' + (temp % 10);
                temp /= 10;
            }
            mode_num[digits] = L'\0';
            print_string(mode_num);

            print_string(L"x");

            temp = mode_info->VerticalResolution;
            digits = 0;
            do {
                digits++;
                temp /= 10;
            } while (temp > 0);
            temp = mode_info->VerticalResolution;
            for (UINT32 j = 0; j < digits; j++) {
                mode_num[digits - 1 - j] = L'0' + (temp % 10);
                temp /= 10;
            }
            mode_num[digits] = L'\0';
            print_string(mode_num);
            print_string(L"\r\n");

// 1920x1080
#ifdef COMPATIBLE_MODE
            if (!found_target && mode_info->HorizontalResolution == 1920 &&
                mode_info->VerticalResolution == 1080) {
                best_mode = i;
                best_width = mode_info->HorizontalResolution;
                best_height = mode_info->VerticalResolution;
                found_target = 1;
            }
#endif

            // 没有锁定目标时，选择分辨率尽量大的 32bpp 模式（且 ≥ 1024x768）
            if (!found_target && mode_info->HorizontalResolution >= 1024 &&
                mode_info->VerticalResolution >= 768) {
                if (mode_info->HorizontalResolution > best_width ||
                    (mode_info->HorizontalResolution == best_width &&
                     mode_info->VerticalResolution > best_height)) {
                    best_mode = i;
                    best_width = mode_info->HorizontalResolution;
                    best_height = mode_info->VerticalResolution;
                }
            }

            gST->BootServices->FreePool(mode_info);
        }

        // 如果没有找到合适的 32bpp 模式，就用当前模式（但这时可能不是 32bpp）
        if (best_width == 0) {
            print_string(
                L"[!] No 32bpp mode found, using current mode as fallback\r\n");
            best_mode = gop->Mode->Mode;
            best_width = gop->Mode->Info->HorizontalResolution;
            best_height = gop->Mode->Info->VerticalResolution;
        } else if (found_target) {
            print_string(L"[->] Using 1920x1080 32bpp mode\r\n");
        } else {
            print_string(L"[->] Choose the best available 32bpp mode: ");

            CHAR16 mode_num[16];
            UINT32 temp = (UINT32)best_mode;
            UINT32 digits = 0;
            do {
                digits++;
                temp /= 10;
            } while (temp > 0);
            temp = (UINT32)best_mode;
            for (UINT32 j = 0; j < digits; j++) {
                mode_num[digits - 1 - j] = L'0' + (temp % 10);
                temp /= 10;
            }
            mode_num[digits] = L'\0';
            print_string(mode_num);

            print_string(L" Resolution: ");

            temp = (UINT32)best_width;
            digits = 0;
            do {
                digits++;
                temp /= 10;
            } while (temp > 0);
            temp = (UINT32)best_width;
            for (UINT32 j = 0; j < digits; j++) {
                mode_num[digits - 1 - j] = L'0' + (temp % 10);
                temp /= 10;
            }
            mode_num[digits] = L'\0';
            print_string(mode_num);

            print_string(L"x");

            temp = (UINT32)best_height;
            digits = 0;
            do {
                digits++;
                temp /= 10;
            } while (temp > 0);
            temp = (UINT32)best_height;
            for (UINT32 j = 0; j < digits; j++) {
                mode_num[digits - 1 - j] = L'0' + (temp % 10);
                temp /= 10;
            }
            mode_num[digits] = L'\0';
            print_string(mode_num);
            print_string(L"\r\n");
        }

        if (best_mode != gop->Mode->Mode) {
            EFI_STATUS st = gop->SetMode(gop, best_mode);
            if (EFI_ERROR(st)) {
                print_error(L"[X] Set graphics mode", st);
                best_mode = gop->Mode->Mode;
                best_width = gop->Mode->Info->HorizontalResolution;
                best_height = gop->Mode->Info->VerticalResolution;
            } else {
                print_string(L"[OK] Set graphics mode\r\n");
            }
        } else {
            print_string(L"[->] Use current graphics mode\r\n");
        }

        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* mode_info = NULL;
        UINTN size_of_info = 0;

        status =
            gop->QueryMode(gop, gop->Mode->Mode, &size_of_info, &mode_info);
        if (!EFI_ERROR(status) && mode_info) {
            // 这里只接受 32bpp 模式
            BOOLEAN is_32bpp = (mode_info->PixelFormat ==
                                PixelBlueGreenRedReserved8BitPerColor) ||
                               (mode_info->PixelFormat ==
                                PixelRedGreenBlueReserved8BitPerColor);

            if (!is_32bpp) {
                print_string(L"[X] Final graphics mode is not 32bpp, fallback "
                             L"to VGA\r\n");
                params->framebuffer_addr = 0xA0000;
                params->framebuffer_width = 320;
                params->framebuffer_height = 200;
                params->framebuffer_pitch = 320;
                params->framebuffer_bpp = 8;
            } else {
                params->framebuffer_addr = (uint64_t)gop->Mode->FrameBufferBase;
                params->framebuffer_width = mode_info->HorizontalResolution;
                params->framebuffer_height = mode_info->VerticalResolution;
                params->framebuffer_pitch =
                    mode_info->PixelsPerScanLine * 4; // 32bpp
                params->framebuffer_bpp = 32;
                params->framebuffer_size = (uint64_t)gop->Mode->FrameBufferSize;

                print_string(L"[OK] Graphics mode configured\r\n");
            }

            gST->BootServices->FreePool(mode_info);
        } else {
            print_error(L"[X] Failed to query graphics mode", status);
            params->framebuffer_addr = 0xA0000;
            params->framebuffer_width = 320;
            params->framebuffer_height = 200;
            params->framebuffer_pitch = 320;
            params->framebuffer_bpp = 8;
        }
    } else {
        // 没有 GOP，退回 VGA
        params->framebuffer_addr = 0xA0000;
        params->framebuffer_width = 320;
        params->framebuffer_height = 200;
        params->framebuffer_pitch = 320;
        params->framebuffer_bpp = 8;

        print_string(L"[!] Using default VGA framebuffer\r\n");
    }

    EFI_MEMORY_DESCRIPTOR* memory_map = NULL;
    UINTN memory_map_size = 0;
    UINTN map_key;
    UINTN descriptor_size = 0;
    UINT32 descriptor_version = 0;

    // 第一次调用：用 NULL buffer 拿到 memory_map_size 和 descriptor_size
    status =
        gST->BootServices->GetMemoryMap(&memory_map_size, memory_map, &map_key,
                                        &descriptor_size, &descriptor_version);

    if (status != EFI_BUFFER_TOO_SMALL) {
        print_error(L"[X] Get memory map size", status);
        return status;
    }

    // 多给两个 descriptor 的余量，防止中间有内存分配变化
    memory_map_size += 2 * descriptor_size;

    status = gST->BootServices->AllocatePool(EfiLoaderData, memory_map_size,
                                             (VOID**)&memory_map);
    if (EFI_ERROR(status)) {
        print_error(L"[X] Allocate memory for memory map", status);
        return status;
    }
    print_string(L"[OK] Allocate memory for memory map\r\n");

    status =
        gST->BootServices->GetMemoryMap(&memory_map_size, memory_map, &map_key,
                                        &descriptor_size, &descriptor_version);
    if (EFI_ERROR(status)) {
        print_error(L"[X] Get memory map", status);
        return status;
    }
    print_string(L"[OK] Get memory map size\r\n");

    params->memory_map_addr = (uint64_t)memory_map;
    params->memory_map_size = (uint64_t)memory_map_size;
    params->descriptor_size = (uint64_t)descriptor_size;

    // 退出 BootServices
    // 如果因为 map 改变导致 EFI_INVALID_PARAMETER，就重试一次
    while (1) {
        // 需要取一次 memory map + map_key
        print_string(L"[!] ExitBootServices invalid parameter, retry\r\n");

        memory_map_size = 0;
        status = gST->BootServices->GetMemoryMap(&memory_map_size, NULL,
                                                 &map_key, &descriptor_size,
                                                 &descriptor_version);
        if (status != EFI_BUFFER_TOO_SMALL) {
            print_error(L"[X] GetMemoryMap (retry size)", status);
            return status;
        }

        // 假设原来的 memory_map buffer 足够大（前面多给了一点）
        status = gST->BootServices->GetMemoryMap(&memory_map_size, memory_map,
                                                 &map_key, &descriptor_size,
                                                 &descriptor_version);
        if (EFI_ERROR(status)) {
            print_error(L"[X] GetMemoryMap (retry)", status);
            return status;
        }

        status = gST->BootServices->ExitBootServices(gImageHandle, map_key);
        if (status == EFI_SUCCESS) {
            break;
        }

        if (status != EFI_INVALID_PARAMETER) {
            print_error(L"[X] ExitBootServices failed", status);
            return status;
        }
    }

    void (*kernel_entry)(void*) = (void (*)(void*))gKernelBase;
    kernel_entry(params);

    return EFI_SUCCESS;
}
void* find_kernel(void) {
    print_string(gKernelBase);
    print_string(L"\r\n");
    return gKernelBase;
}