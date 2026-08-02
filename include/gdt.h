#pragma once
#include <stdint.h>

// gdt条目(for 64-bit mode)
// access byte: P=1,DPL=2,DC=1,E=1,C=1,R=1,A=1
//   - kernel code: 0x9A (P=1,DPL=0,code=1,readable=1)
//   - kernel data: 0x92 (P=1,DPL=0,data=1,writable=1)
//   - user   code: 0xFA (P=1,DPL=3,code=1,readable=1)
//   - user   data: 0xF2 (P=1,DPL=3,data=1,writable=1)
// gran byte: G=1,D=0,L=1,AVL=0 => 0xA0 for 64-bit code
//            G=1,D=1,L=0,AVL=0 => 0xC0 for 32-bit data
//            G=0 for TSS (limit in bytes)

#define GDT_KCODE 1
#define GDT_KDATA 2
#define GDT_UDATA 3
#define GDT_UCODE 4
#define GDT_TSS   5
#define GDT_ENTRIES 7   // tss descriptor占2个entry(num 5 + num 6)

#define SEL_KCODE ((GDT_KCODE) * 8)       // 0x08
#define SEL_KDATA ((GDT_KDATA) * 8)       // 0x10
#define SEL_UDATA ((GDT_UDATA) * 8 | 3)   // 0x1B  (user data)
#define SEL_UCODE ((GDT_UCODE) * 8 | 3)   // 0x23  (user code)
#define SEL_TSS   ((GDT_TSS)   * 8)       // 0x28

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

// 64-bit TSS结构
struct tss {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint32_t reserved1;
    uint32_t reserved2;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint32_t reserved3;
    uint32_t reserved4;
    uint16_t reserved5;
    uint16_t iomap_base;
} __attribute__((packed));

void gdt_init(void);
void tss_init(void);
void tss_set_stack(uint64_t rsp0);

// 进入用户态(通过iretq)
void enter_usermode(uint64_t entry, uint64_t stack);
