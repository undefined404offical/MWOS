#include "bootloader.h"

EFI_SYSTEM_TABLE *gST = NULL;

// UEFI 应用程序入口点
EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    gST = SystemTable;
    
    set_image_handle(ImageHandle);
    
    gST->ConOut->Reset(gST->ConOut, 0);
    
    gST->ConOut->OutputString(gST->ConOut, L"MWOS UEFI Bootloader\r\n");
    gST->ConOut->OutputString(gST->ConOut, L"=====================\r\n\r\n");
    
    EFI_STATUS status = load_kernel();
    if (EFI_ERROR(status)) {
        print_error(L"[X] Load kernel", status);
    } else {
        print_string(L"[OK] Load kernel\r\n");
    }

    status = boot_kernel();

    return EFI_SUCCESS;
}