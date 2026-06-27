#include "kernel.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

static uint8_t *bitmap;
static uint64_t total_pages;
static uint64_t free_pages;
static uint64_t bitmap_size;

#define PAGE_SIZE 4096ULL

#define SET_BIT(i) (bitmap[(i) / 8] |= (uint8_t)(1u << ((i) % 8)))
#define CLEAR_BIT(i) (bitmap[(i) / 8] &= (uint8_t)~(1u << ((i) % 8)))
#define TEST_BIT(i) (bitmap[(i) / 8] & (uint8_t)(1u << ((i) % 8)))

static bool pmm_is_address_valid(uint64_t addr)
{
    if (addr == 0 || addr == 0xFFFFFFFFFFFFFFFFULL)
        return false;
    // 简单上限保护：> 0x8000_0000_0000 认为无效
    if (addr > 0x0000008000000000ULL)
        return false;
    return true;
}

void pmm_init(void *mmap, size_t mmap_size, size_t desc_size)
{
    if (desc_size == 0)
    {
        serial_puts("FATAL: desc_size is ZERO!\n");
        while (1)
        {
        }
    }

    uint64_t max_addr = 0;
    size_t desc_count = mmap_size / desc_size;
    uint8_t *mmap_ptr = (uint8_t *)mmap;

    // 1) 找到最高物理地址，确定 total_pages / bitmap_size
    for (size_t i = 0; i < desc_count; i++)
    {
        efi_mem_desc_t *d = (efi_mem_desc_t *)(mmap_ptr + i * desc_size);
        uint64_t end = d->physical_start + d->number_of_pages * PAGE_SIZE;
        if (end > max_addr)
            max_addr = end;
    }

    total_pages = max_addr / PAGE_SIZE;
    bitmap_size = (total_pages + 7) / 8; // 向上取整到字节
    bitmap = NULL;

    // 2) 在一个足够大的 EfiConventionalMemory 区域里放 bitmap
    for (size_t i = 0; i < desc_count; i++)
    {
        efi_mem_desc_t *d = (efi_mem_desc_t *)(mmap_ptr + i * desc_size);
        // 类型 7: EfiConventionalMemory
        if (d->type == 7 && (d->number_of_pages * PAGE_SIZE) >= bitmap_size)
        {
            // 避开低地址，至少放在 16MB 之后
            if (d->physical_start >= 0x01000000ULL)
            {
                bitmap = (uint8_t *)d->physical_start;
                break;
            }
        }
    }

    if (!bitmap)
    {
        serial_puts("FATAL: cannot find space for PMM bitmap\n");
        while (1)
        {
        }
    }

    // 3) 初始全部标记为“已占用”
    memset(bitmap, 0xFF, bitmap_size);
    free_pages = 0;

    // 4) 遍历内存映射，把可用页标记为“空闲”
    //    用“按页循环”的方式，保证逻辑简单正确
    for (size_t i = 0; i < desc_count; i++)
    {
        efi_mem_desc_t *d = (efi_mem_desc_t *)(mmap_ptr + i * desc_size);

        // 你原来允许的类型：7(EfiConventionalMemory)、3、4
        if (d->type == 7 || d->type == 3 || d->type == 4)
        {
            uint64_t start_page = d->physical_start / PAGE_SIZE;
            uint64_t end_page = start_page + d->number_of_pages;

            if (end_page > total_pages)
                end_page = total_pages;
            if (start_page >= end_page)
                continue;

            for (uint64_t p = start_page; p < end_page; p++)
            {
                // 先假定所有页都可用，后面再做“内核保护”和“bitmap 自保护”
                if (TEST_BIT(p))
                {
                    CLEAR_BIT(p);
                    free_pages++;
                }
            }
        }
    }

    // 5) 保护前 16MB 内核区：强制标记为“已占用”
    uint64_t kernel_safety_pages = 0x01000000ULL / PAGE_SIZE;
    for (uint64_t i = 0; i < kernel_safety_pages && i < total_pages; i++)
    {
        if (!TEST_BIT(i))
        {
            SET_BIT(i);
            free_pages--;
        }
    }

    // 6) 保护 bitmap 自身所在的页
    uint64_t bitmap_start_page = (uint64_t)bitmap / PAGE_SIZE;
    uint64_t bitmap_pages = (bitmap_size + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint64_t i = 0; i < bitmap_pages; i++)
    {
        uint64_t page_index = bitmap_start_page + i;
        if (page_index < total_pages && !TEST_BIT(page_index))
        {
            SET_BIT(page_index);
            free_pages--;
        }
    }

    serial_puts("pmm: initialized. total_pages=");
    serial_putdec64(total_pages);
    serial_puts(" free_pages=");
    serial_putdec64(free_pages);
    serial_puts("\n");
}

void *pmm_alloc_page(void)
{
    // 从 16MB 之后开始找，避免踩内核
    uint64_t start_search = 0x01000000ULL / PAGE_SIZE;

    for (uint64_t i = start_search; i < total_pages; i++)
    {
        if (!TEST_BIT(i))
        {
            uint64_t addr = i * PAGE_SIZE;
            if (!pmm_is_address_valid(addr))
                continue;

            SET_BIT(i);
            free_pages--;
            return (void *)addr;
        }
    }

    return NULL;
}

void *pmm_alloc_zpage(void)
{
    void *addr = pmm_alloc_page();
    if (addr)
        memset(addr, 0, PAGE_SIZE);
    return addr;
}

void *pmm_alloc_blocks(size_t count)
{
    if (count == 0)
        return NULL;
    if (count > free_pages)
        return NULL;

    uint64_t start_search = 0x01000000ULL / PAGE_SIZE;
    uint64_t consecutive = 0;
    uint64_t start_index = 0;

    for (uint64_t i = start_search; i < total_pages; i++)
    {
        if (!TEST_BIT(i))
        {
            if (consecutive == 0)
                start_index = i;
            consecutive++;

            if (consecutive == count)
            {
                // 找到了 [start_index, start_index + count) 这一段连续空闲页
                for (uint64_t p = start_index; p < start_index + count; p++)
                {
                    SET_BIT(p);
                }
                free_pages -= count;
                return (void *)(start_index * PAGE_SIZE);
            }
        }
        else
        {
            consecutive = 0;
        }
    }

    return NULL;
}

void pmm_free_page(void *addr)
{
    if (!addr)
        return;

    uint64_t page_index = (uint64_t)addr / PAGE_SIZE;

    // 不允许释放 16MB 以内的页
    if (page_index < (0x01000000ULL / PAGE_SIZE))
        return;
    if (page_index >= total_pages)
        return;

    if (TEST_BIT(page_index))
    {
        CLEAR_BIT(page_index);
        free_pages++;
    }
}

void pmm_free_blocks(void *addr, size_t count)
{
    if (!addr || count == 0)
        return;

    uint64_t start_index = (uint64_t)addr / PAGE_SIZE;

    for (uint64_t i = 0; i < count; i++)
    {
        uint64_t page_index = start_index + i;
        if (page_index >= total_pages)
            break;

        // 复用单页释放逻辑
        if (page_index >= (0x01000000ULL / PAGE_SIZE) && TEST_BIT(page_index))
        {
            CLEAR_BIT(page_index);
            free_pages++;
        }
    }
}

void pmm_reserve_area(uint64_t start, size_t size)
{
    if (start == 0 || size == 0)
        return;

    uint64_t start_page = start / PAGE_SIZE;
    uint64_t end_page = (start + size + PAGE_SIZE - 1) / PAGE_SIZE;

    if (start_page >= total_pages)
        return;
    if (end_page > total_pages)
        end_page = total_pages;

    size_t reserved = 0;
    for (uint64_t i = start_page; i < end_page; i++)
    {
        if (!TEST_BIT(i))
        {
            SET_BIT(i);
            free_pages--;
            reserved++;
        }
    }

    serial_puts("pmm: reserved area [0x");
    serial_puthex64(start);
    serial_puts("-0x");
    serial_puthex64(start + size);
    serial_puts("] ");
    serial_putdec64(reserved);
    serial_puts(" pages\n");
}
