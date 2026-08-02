#include "gdt.h"
#include "serial.h"
#include <string.h>

static struct gdt_entry gdt[GDT_ENTRIES];
static struct gdt_ptr gdtr;
static struct tss tss_entry;

// 设置一个gdt条目(for 64-bit)
static void gdt_set_entry(int num, uint32_t base, uint32_t limit,
                          uint8_t access, uint8_t gran)
{
    gdt[num].limit_low   = limit & 0xFFFF;
    gdt[num].base_low    = base & 0xFFFF;
    gdt[num].base_middle = (base >> 16) & 0xFF;
    gdt[num].access      = access;
    gdt[num].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[num].base_high   = (base >> 24) & 0xFF;
}

// 设置一个tss描述符(system segment, x86_64占2个entry)
static void gdt_set_tss(int num, uint64_t base, uint32_t limit)
{
    gdt[num].limit_low   = limit & 0xFFFF;
    gdt[num].base_low    = base & 0xFFFF;
    gdt[num].base_middle = (base >> 16) & 0xFF;
    gdt[num].access      = 0x89;  // P=1,DPL=0,type=available TSS
    gdt[num].granularity = ((limit >> 16) & 0x0F) | 0x00;
    gdt[num].base_high   = (base >> 24) & 0xFF;
    // upper 32 bits of base go into the next entry
    uint32_t *p = (uint32_t*)&gdt[num + 1];
    p[0] = (uint32_t)(base >> 32);
    p[1] = 0;
}

// 加载tss (在内核段执行)
static void tss_flush(void)
{
    asm volatile("ltr %0" : : "r"((uint16_t)SEL_TSS));
}

void gdt_init(void)
{
    memset(gdt, 0, sizeof(gdt));

    gdt_set_entry(0, 0, 0, 0, 0);                // null
    gdt_set_entry(GDT_KCODE, 0, 0, 0x9A, 0xA0); // kernel code 64-bit
    gdt_set_entry(GDT_KDATA, 0, 0, 0x92, 0x00); // kernel data
    gdt_set_entry(GDT_UCODE, 0, 0, 0xFA, 0xA0); // user code 64-bit (at index 4)
    gdt_set_entry(GDT_UDATA, 0, 0, 0xF2, 0x00); // user data (at index 3)

    // TSS descriptor (gdt[5..6])
    gdt_set_tss(GDT_TSS, (uint64_t)&tss_entry, sizeof(struct tss) - 1);

    // gdt_ptr: TSS占用2个entry, 所以总大小是(GDT_TSS+2)*8
    gdtr.limit = (sizeof(struct gdt_entry) * (GDT_TSS + 2)) - 1;
    gdtr.base = (uint64_t)&gdt;

    asm volatile(
        "lgdt %0\n\t"
        "push %1\n\t"
        "lea 1f(%%rip), %%rax\n\t"
        "push %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        "mov %2, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%ss\n\t"
        :
        : "m"(gdtr),
          "i"(SEL_KCODE),
          "i"(SEL_KDATA)
        : "rax");

    tss_flush();
    serial_puts("gdt: initialized (6 entries + tss)\n");
}

// 初始化tss
void tss_init(void)
{
    memset(&tss_entry, 0, sizeof(tss_entry));
    tss_entry.iomap_base = sizeof(struct tss);
}

// 设置中断时切换到的内核栈
void tss_set_stack(uint64_t rsp0)
{
    tss_entry.rsp0 = rsp0;
}
