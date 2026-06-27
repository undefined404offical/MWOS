#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <elf.h>
#include "exec/mwp_format.h"

#define MAX_SECTIONS 16
#define MAX_SYMBOLS 256
#define MAX_STRINGS 4096

typedef struct {
    uint8_t *data;
    size_t size;
} file_data_t;

typedef struct {
    char name[64];
    uint32_t type;
    uint64_t vaddr;
    uint32_t file_offset;
    uint32_t file_size;
    uint32_t mem_size;
    uint32_t flags;
    uint32_t align;
    uint8_t *data;
} output_section_t;

typedef struct {
    char name[256];
    uint32_t name_offset;
    uint8_t bind;
    uint8_t type;
    uint16_t section_index;
    uint64_t value;
    uint64_t size;
} output_symbol_t;

static output_section_t sections[MAX_SECTIONS];
static int section_count = 0;

static output_symbol_t symbols[MAX_SYMBOLS];
static int symbol_count = 0;

static char string_table[MAX_STRINGS];
static int string_table_size = 1;

static uint32_t add_to_string_table(const char *str) {
    uint32_t offset = string_table_size;
    int len = strlen(str) + 1;
    if (string_table_size + len > MAX_STRINGS) {
        fprintf(stderr, "Error: string table overflow\n");
        exit(1);
    }
    strcpy(string_table + string_table_size, str);
    string_table_size += len;
    return offset;
}

static uint32_t get_section_flags(uint32_t elf_flags) {
    uint32_t mwp_flags = 0;
    if (elf_flags & SHF_WRITE) mwp_flags |= MWP_SECTION_WRITE;
    if (elf_flags & SHF_ALLOC) mwp_flags |= MWP_SECTION_READ;
    if (elf_flags & SHF_EXECINSTR) mwp_flags |= MWP_SECTION_EXEC;
    return mwp_flags;
}

static uint32_t get_section_type(uint32_t elf_type) {
    switch (elf_type) {
        case SHT_PROGBITS: return MWP_SECTION_TEXT;
        case SHT_NOBITS: return MWP_SECTION_BSS;
        default: return MWP_SECTION_NULL;
    }
}

static const char *get_section_name_from_elf(const char *strtab, Elf64_Shdr *shdr) {
    return strtab + shdr->sh_name;
}

static void add_section(const char *name, uint32_t type, uint64_t vaddr,
                       uint32_t file_size, uint32_t mem_size, uint32_t flags,
                       uint32_t align, uint8_t *data) {
    if (section_count >= MAX_SECTIONS) {
        fprintf(stderr, "Error: too many sections\n");
        exit(1);
    }
    
    output_section_t *sec = &sections[section_count];
    strcpy(sec->name, name);
    sec->type = type;
    sec->vaddr = vaddr;
    sec->file_offset = 0;
    sec->file_size = file_size;
    sec->mem_size = mem_size;
    sec->flags = flags;
    sec->align = align;
    sec->data = data;
    
    section_count++;
}

static void add_symbol(const char *name, uint8_t bind, uint8_t type,
                      uint16_t section_index, uint64_t value, uint64_t size) {
    if (symbol_count >= MAX_SYMBOLS) {
        fprintf(stderr, "Error: too many symbols\n");
        exit(1);
    }
    
    output_symbol_t *sym = &symbols[symbol_count];
    strcpy(sym->name, name);
    sym->name_offset = add_to_string_table(name);
    sym->bind = bind;
    sym->type = type;
    sym->section_index = section_index;
    sym->value = value;
    sym->size = size;
    
    symbol_count++;
}

static file_data_t read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Error: cannot open file '%s'\n", path);
        exit(1);
    }
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    uint8_t *data = malloc(size);
    if (!data) {
        fprintf(stderr, "Error: out of memory\n");
        exit(1);
    }
    
    fread(data, 1, size, f);
    fclose(f);
    
    file_data_t result = {data, (size_t)size};
    return result;
}

static void write_file(const char *path, uint8_t *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "Error: cannot create file '%s'\n", path);
        exit(1);
    }
    
    fwrite(data, 1, size, f);
    fclose(f);
}

static int find_or_create_section_index(const char *name) {
    for (int i = 0; i < section_count; i++) {
        if (strcmp(sections[i].name, name) == 0) {
            return i + 1;
        }
    }
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <input.elf> -o <output.mwp>\n", argv[0]);
        return 1;
    }
    
    const char *input_path = argv[1];
    const char *output_path = NULL;
    
    for (int i = 2; i < argc - 1; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            output_path = argv[i + 1];
            break;
        }
    }
    
    if (!output_path) {
        fprintf(stderr, "Error: output file not specified\n");
        return 1;
    }
    
    file_data_t elf_file = read_file(input_path);
    
    if (elf_file.size < sizeof(Elf64_Ehdr)) {
        fprintf(stderr, "Error: file too small to be ELF\n");
        free(elf_file.data);
        return 1;
    }
    
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)elf_file.data;
    
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) {
        fprintf(stderr, "Error: not an ELF file\n");
        free(elf_file.data);
        return 1;
    }
    
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64 ||
        ehdr->e_ident[EI_DATA] != ELFDATA2LSB ||
        ehdr->e_machine != EM_X86_64) {
        fprintf(stderr, "Error: not a valid x86_64 ELF file\n");
        free(elf_file.data);
        return 1;
    }
    
    Elf64_Shdr *shdr = (Elf64_Shdr *)(elf_file.data + ehdr->e_shoff);
    Elf64_Shdr *shstrtab_shdr = &shdr[ehdr->e_shstrndx];
    const char *shstrtab = (const char *)(elf_file.data + shstrtab_shdr->sh_offset);
    
    Elf64_Shdr *symtab_shdr = NULL;
    Elf64_Shdr *strtab_shdr = NULL;
    const char *strtab = NULL;
    
    for (int i = 0; i < ehdr->e_shnum; i++) {
        const char *name = shstrtab + shdr[i].sh_name;
        
        if (shdr[i].sh_type == SHT_SYMTAB) {
            symtab_shdr = &shdr[i];
            strtab_shdr = &shdr[shdr[i].sh_link];
            strtab = (const char *)(elf_file.data + strtab_shdr->sh_offset);
        }
        
        if (shdr[i].sh_type == SHT_PROGBITS || shdr[i].sh_type == SHT_NOBITS) {
            if (strcmp(name, ".text") == 0 || strcmp(name, ".rodata") == 0 ||
                strcmp(name, ".data") == 0 || strcmp(name, ".bss") == 0) {
                
                uint8_t *data = NULL;
                if (shdr[i].sh_type != SHT_NOBITS) {
                    data = elf_file.data + shdr[i].sh_offset;
                }
                
                uint32_t type;
                if (strcmp(name, ".text") == 0) type = MWP_SECTION_TEXT;
                else if (strcmp(name, ".rodata") == 0) type = MWP_SECTION_RODATA;
                else if (strcmp(name, ".data") == 0) type = MWP_SECTION_DATA;
                else type = MWP_SECTION_BSS;
                
                add_section(name, type, shdr[i].sh_addr,
                           shdr[i].sh_size, shdr[i].sh_size,
                           get_section_flags(shdr[i].sh_flags),
                           shdr[i].sh_addralign, data);
            }
        }
    }
    
    if (symtab_shdr && strtab) {
        Elf64_Sym *sym = (Elf64_Sym *)(elf_file.data + symtab_shdr->sh_offset);
        int num_syms = symtab_shdr->sh_size / sizeof(Elf64_Sym);
        
        for (int i = 0; i < num_syms; i++) {
            const char *name = strtab + sym[i].st_name;
            if (name[0] == '\0') continue;
            
            uint8_t bind = ELF64_ST_BIND(sym[i].st_info);
            uint8_t type = ELF64_ST_TYPE(sym[i].st_info);
            
            uint16_t section_index = 0;
            if (sym[i].st_shndx < ehdr->e_shnum) {
                const char *sec_name = shstrtab + shdr[sym[i].st_shndx].sh_name;
                section_index = find_or_create_section_index(sec_name);
            }
            
            uint8_t mwp_type = MWP_SYM_UNDEF;
            if (type == STT_FUNC) mwp_type = MWP_SYM_FUNC;
            else if (type == STT_OBJECT) mwp_type = MWP_SYM_DATA;
            
            add_symbol(name, bind == STB_GLOBAL ? MWP_BIND_GLOBAL : MWP_BIND_LOCAL,
                      mwp_type, section_index, sym[i].st_value, sym[i].st_size);
        }
    }
    
    uint32_t header_size = sizeof(mwp_header_t);
    uint32_t section_table_size = section_count * sizeof(mwp_section_header_t);
    uint32_t symbol_table_size = symbol_count * sizeof(mwp_symbol_t);
    uint32_t aligned_str_table = (string_table_size + 3) & ~3;
    
    uint32_t data_offset = header_size + section_table_size + symbol_table_size + aligned_str_table;
    data_offset = (data_offset + 15) & ~15;
    
    mwp_header_t mwp_header;
    memset(&mwp_header, 0, sizeof(mwp_header));
    mwp_header.magic = MWP_MAGIC;
    mwp_header.version = MWP_VERSION;
    mwp_header.type = MWP_TYPE_EXECUTABLE;
    mwp_header.machine = MWP_MACHINE_X86_64;
    mwp_header.section_count = section_count;
    mwp_header.symbol_count = symbol_count;
    mwp_header.string_table_offset = header_size + section_table_size + symbol_table_size;
    mwp_header.string_table_size = string_table_size;
    
    Elf64_Shdr *entry_shdr = NULL;
    for (int i = 0; i < ehdr->e_shnum; i++) {
        const char *name = shstrtab + shdr[i].sh_name;
        if (strcmp(name, ".text") == 0) {
            entry_shdr = &shdr[i];
            break;
        }
    }
    
    uint64_t entry_addr = ehdr->e_entry;
    if (entry_shdr) {
        mwp_header.entry_point = entry_addr - entry_shdr->sh_addr + 
                                 (header_size + section_table_size + symbol_table_size + aligned_str_table);
        mwp_header.entry_point = (mwp_header.entry_point + 15) & ~15;
    }
    
    uint32_t current_offset = data_offset;
    for (int i = 0; i < section_count; i++) {
        sections[i].file_offset = current_offset;
        if (sections[i].type != MWP_SECTION_BSS) {
            current_offset += sections[i].file_size;
        }
        current_offset = (current_offset + 15) & ~15;
    }
    
    uint32_t total_size = current_offset;
    uint8_t *mwp_data = calloc(1, total_size);
    if (!mwp_data) {
        fprintf(stderr, "Error: out of memory\n");
        free(elf_file.data);
        return 1;
    }
    
    uint32_t offset = 0;
    memcpy(mwp_data + offset, &mwp_header, sizeof(mwp_header));
    offset += sizeof(mwp_header);
    
    for (int i = 0; i < section_count; i++) {
        mwp_section_header_t sec_hdr;
        sec_hdr.name_offset = add_to_string_table(sections[i].name);
        sec_hdr.type = sections[i].type;
        sec_hdr.virtual_addr = sections[i].vaddr;
        sec_hdr.file_offset = sections[i].file_offset;
        sec_hdr.file_size = sections[i].type == MWP_SECTION_BSS ? 0 : sections[i].file_size;
        sec_hdr.mem_size = sections[i].mem_size;
        sec_hdr.flags = sections[i].flags;
        sec_hdr.align = sections[i].align;
        
        memcpy(mwp_data + offset, &sec_hdr, sizeof(mwp_section_header_t));
        offset += sizeof(mwp_section_header_t);
    }
    
    for (int i = 0; i < symbol_count; i++) {
        mwp_symbol_t sym_hdr;
        sym_hdr.name_offset = symbols[i].name_offset;
        sym_hdr.bind = symbols[i].bind;
        sym_hdr.type = symbols[i].type;
        sym_hdr.section_index = symbols[i].section_index;
        sym_hdr.value = symbols[i].value;
        sym_hdr.size = symbols[i].size;
        
        memcpy(mwp_data + offset, &sym_hdr, sizeof(mwp_symbol_t));
        offset += sizeof(mwp_symbol_t);
    }
    
    memcpy(mwp_data + offset, string_table, string_table_size);
    offset += aligned_str_table;
    
    for (int i = 0; i < section_count; i++) {
        if (sections[i].type != MWP_SECTION_BSS && sections[i].data) {
            memcpy(mwp_data + sections[i].file_offset, sections[i].data, sections[i].file_size);
        }
    }
    
    write_file(output_path, mwp_data, total_size);
    
    printf("MWP file created: %s\n", output_path);
    printf("  Sections: %d\n", section_count);
    printf("  Symbols: %d\n", symbol_count);
    printf("  Size: %u bytes\n", total_size);
    
    free(elf_file.data);
    free(mwp_data);
    
    return 0;
}
