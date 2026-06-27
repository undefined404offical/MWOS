#include "exec/mwp_loader.h"
#include "drivers/fs/fat32.h"
#include "klog.h"
#include "memory.h"
#include "serial.h"
#include "string.h"
#include "thread.h"

static mwp_exported_symbol_t exported_symbols[MWP_MAX_EXPORTED_SYMBOLS];
static int exported_symbol_count = 0;

static void* find_exported_symbol(const char* name) {
    for (int i = 0; i < exported_symbol_count; i++) {
        if (strcmp(exported_symbols[i].name, name) == 0) {
            return exported_symbols[i].address;
        }
    }
    return NULL;
}

void mwp_register_symbol(const char* name, void* addr) {
    if (exported_symbol_count >= MWP_MAX_EXPORTED_SYMBOLS) {
        kerror("mwp: too many exported symbols\n");
        return;
    }

    strncpy(exported_symbols[exported_symbol_count].name, name, 63);
    exported_symbols[exported_symbol_count].name[63] = '\0';
    exported_symbols[exported_symbol_count].address = addr;
    exported_symbol_count++;
}

void mwp_kprintf(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    serial_puts(buf);
}

void mwp_init_exports(void) {
    mwp_register_symbol("kprintf", mwp_kprintf);
    mwp_register_symbol("serial_puts", serial_puts);
    mwp_register_symbol("kmalloc", kmalloc);
    mwp_register_symbol("kfree", kfree);
    mwp_register_symbol("fat32_open", fat32_open);
    mwp_register_symbol("fat32_read", fat32_read);
    mwp_register_symbol("fat32_write", fat32_write);
    mwp_register_symbol("fat32_close", fat32_close);
    mwp_register_symbol("thread_create", thread_create);
}

static int mwp_resolve_symbols(mwp_program_t* prog) {
    for (int i = 0; i < prog->header.symbol_count; i++) {
        mwp_symbol_t* sym = &prog->symbols[i];

        if (sym->section_index == 0 && sym->bind == MWP_BIND_GLOBAL) {
            void* addr =
                find_exported_symbol(prog->string_table + sym->name_offset);
            if (!addr) {
                kerror("mwp: unresolved symbol: ");
                kerror("%s", prog->string_table + sym->name_offset);
                kerror("%n");
                return -1;
            }
            sym->value = (uint64_t)addr;
        }
    }
    return 0;
}

static int mwp_apply_relocations(mwp_program_t* prog) {
    for (int i = 0; i < prog->header.symbol_count; i++) {
        mwp_symbol_t* sym = &prog->symbols[i];

        if (sym->section_index > 0 && sym->bind == MWP_BIND_GLOBAL) {
            uint64_t sym_addr = (uint64_t)prog->load_address + sym->value;

            for (int j = 0; j < prog->header.section_count; j++) {
                mwp_section_header_t* sec = &prog->sections[j];

                if (sec->type == MWP_SECTION_TEXT ||
                    sec->type == MWP_SECTION_DATA) {
                    uint8_t* section_data =
                        (uint8_t*)prog->load_address + sec->file_offset;

                    for (uint32_t offset = 0; offset < sec->file_size - 8;
                         offset += 4) {
                        uint64_t* potentially_ref =
                            (uint64_t*)(section_data + offset);
                        if (*potentially_ref == sym->value) {
                            *potentially_ref = sym_addr;
                        }
                    }
                }
            }
        }
    }
    return 0;
}

mwp_program_t* mwp_load(const char* path) {
    serial_puts("mwp: loading: ");
    serial_puts(path);
    serial_puts("\n");

    fat32_handle_t handle;
    if (!fat32_open(path, &handle, FILE_READ)) {
        kerror("mwp: failed to open file\n");
        return NULL;
    }

    uint32_t file_size = handle.file_size;
    uint8_t* file_data = kmalloc(file_size);
    if (!file_data) {
        kerror("mwp: failed to allocate memory for file\n");
        fat32_close(&handle);
        return NULL;
    }

    if (!fat32_read(&handle, file_data, file_size)) {
        kerror("mwp: failed to read file\n");
        kfree(file_data);
        fat32_close(&handle);
        return NULL;
    }
    fat32_close(&handle);

    if (file_size < sizeof(mwp_header_t)) {
        kerror("mwp: file too small\n");
        kfree(file_data);
        return NULL;
    }

    mwp_header_t* header = (mwp_header_t*)file_data;

    if (header->magic != MWP_MAGIC) {
        kerror("mwp: invalid magic number\n");
        kfree(file_data);
        return NULL;
    }

    if (header->version != MWP_VERSION) {
        kerror("mwp: unsupported version\n");
        kfree(file_data);
        return NULL;
    }

    if (header->machine != MWP_MACHINE_X86_64) {
        kerror("mwp: unsupported machine type\n");
        kfree(file_data);
        return NULL;
    }

    mwp_program_t* prog = kmalloc(sizeof(mwp_program_t));
    if (!prog) {
        kerror("mwp: failed to allocate program struct\n");
        kfree(file_data);
        return NULL;
    }

    memset(prog, 0, sizeof(mwp_program_t));
    prog->header = *header;

    uint32_t section_table_offset = sizeof(mwp_header_t);
    uint32_t symbol_table_offset =
        section_table_offset +
        header->section_count * sizeof(mwp_section_header_t);
    uint32_t string_table_offset =
        symbol_table_offset + header->symbol_count * sizeof(mwp_symbol_t);

    prog->sections =
        kmalloc(header->section_count * sizeof(mwp_section_header_t));
    if (!prog->sections) {
        serial_puts("mwp: failed to allocate section table\n");
        kfree(prog);
        kfree(file_data);
        return NULL;
    }
    memcpy(prog->sections, file_data + section_table_offset,
           header->section_count * sizeof(mwp_section_header_t));

    prog->symbols = kmalloc(header->symbol_count * sizeof(mwp_symbol_t));
    if (!prog->symbols) {
        kerror("mwp: failed to allocate symbol table\n");
        kfree(prog->sections);
        kfree(prog);
        kfree(file_data);
        return NULL;
    }
    memcpy(prog->symbols, file_data + symbol_table_offset,
           header->symbol_count * sizeof(mwp_symbol_t));

    prog->string_table = kmalloc(header->string_table_size);
    if (!prog->string_table) {
        kerror("mwp: failed to allocate string table\n");
        kfree(prog->symbols);
        kfree(prog->sections);
        kfree(prog);
        kfree(file_data);
        return NULL;
    }
    memcpy(prog->string_table, file_data + string_table_offset,
           header->string_table_size);

    uint32_t total_mem_size = 0;
    for (int i = 0; i < header->section_count; i++) {
        if (prog->sections[i].type == MWP_SECTION_BSS) {
            total_mem_size += prog->sections[i].mem_size;
        } else {
            total_mem_size += prog->sections[i].file_size;
        }
        total_mem_size = (total_mem_size + 15) & ~15;
    }

    prog->total_size = total_mem_size;
    prog->load_address = kmalloc(total_mem_size);
    if (!prog->load_address) {
        kerror("mwp: failed to allocate memory for program\n");
        kfree(prog->string_table);
        kfree(prog->symbols);
        kfree(prog->sections);
        kfree(prog);
        kfree(file_data);
        return NULL;
    }
    memset(prog->load_address, 0, total_mem_size);

    uint32_t offset = 0;
    for (int i = 0; i < header->section_count; i++) {
        mwp_section_header_t* sec = &prog->sections[i];

        if (sec->type != MWP_SECTION_BSS && sec->file_size > 0) {
            memcpy((uint8_t*)prog->load_address + offset,
                   file_data + sec->file_offset, sec->file_size);
        }

        sec->file_offset = offset;
        offset +=
            (sec->type == MWP_SECTION_BSS) ? sec->mem_size : sec->file_size;
        offset = (offset + 15) & ~15;
    }

    kfree(file_data);

    if (mwp_resolve_symbols(prog) < 0) {
        kerror("mwp: symbol resolution failed\n");
        kfree(prog->load_address);
        kfree(prog->string_table);
        kfree(prog->symbols);
        kfree(prog->sections);
        kfree(prog);
        return NULL;
    }

    mwp_apply_relocations(prog);

    prog->entry_address = (uint64_t)prog->load_address + header->entry_point;

    serial_puts("mwp: loaded successfully\n");
    serial_puts("  entry: ");
    serial_puthex64(prog->entry_address);
    serial_puts("\n");
    serial_puts("  size: ");
    serial_putdec64(total_mem_size);
    serial_puts(" bytes\n");

    return prog;
}

static void mwp_program_entry(void* arg) {
    mwp_program_t* prog = (mwp_program_t*)arg;

    typedef int (*mwp_main_t)(int, char**);
    mwp_main_t main_func = (mwp_main_t)prog->entry_address;

    kinfo("mwp: executing program\n");

    int result = main_func(0, NULL);

    kinfo("mwp: program exited with code ");
    serial_putdec64(result);
    kinfo("\n");
}

int mwp_execute(mwp_program_t* prog) {
    if (!prog) {
        return -1;
    }

    thread_create(mwp_program_entry, prog, 10);

    return 0;
}

void mwp_unload(mwp_program_t* prog) {
    if (!prog) {
        return;
    }

    kinfo("mwp: unloading program\n");

    if (prog->load_address) {
        kfree(prog->load_address);
    }
    if (prog->string_table) {
        kfree(prog->string_table);
    }
    if (prog->symbols) {
        kfree(prog->symbols);
    }
    if (prog->sections) {
        kfree(prog->sections);
    }
    kfree(prog);
}
