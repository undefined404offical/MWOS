#ifndef MWP_FORMAT_H
#define MWP_FORMAT_H

#include <stdint.h>

#define MWP_MAGIC 0x4D575000
#define MWP_VERSION 1
#define MWP_MACHINE_X86_64 0x01

#define MWP_TYPE_EXECUTABLE 0x01
#define MWP_TYPE_RELOCATABLE 0x02

#define MWP_FLAG_FPU 0x01
#define MWP_FLAG_GUI 0x02
#define MWP_FLAG_SHELL 0x04

#define MWP_SECTION_NULL 0
#define MWP_SECTION_TEXT 1
#define MWP_SECTION_DATA 2
#define MWP_SECTION_RODATA 3
#define MWP_SECTION_BSS 4
#define MWP_SECTION_SYMTAB 5

#define MWP_SECTION_READ 0x1
#define MWP_SECTION_WRITE 0x2
#define MWP_SECTION_EXEC 0x4

#define MWP_BIND_LOCAL 0
#define MWP_BIND_GLOBAL 1

#define MWP_SYM_UNDEF 0
#define MWP_SYM_FUNC 1
#define MWP_SYM_DATA 2

#pragma pack(push, 1)

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint16_t machine;
    uint16_t section_count;
    uint16_t symbol_count;
    uint32_t entry_point;
    uint32_t string_table_offset;
    uint32_t string_table_size;
    uint32_t flags;
    uint64_t reserved;
} mwp_header_t;

typedef struct {
    uint32_t name_offset;
    uint32_t type;
    uint64_t virtual_addr;
    uint32_t file_offset;
    uint32_t file_size;
    uint32_t mem_size;
    uint32_t flags;
    uint32_t align;
} mwp_section_header_t;

typedef struct {
    uint32_t name_offset;
    uint8_t  bind;
    uint8_t  type;
    uint16_t section_index;
    uint64_t value;
    uint64_t size;
} mwp_symbol_t;

#pragma pack(pop)

#endif
