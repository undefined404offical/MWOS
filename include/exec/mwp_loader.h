#ifndef MWP_LOADER_H
#define MWP_LOADER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "exec/mwp_format.h"

#define MWP_MAX_EXPORTED_SYMBOLS 64

typedef struct {
    char name[64];
    void *address;
} mwp_exported_symbol_t;

typedef struct {
    mwp_header_t header;
    mwp_section_header_t *sections;
    mwp_symbol_t *symbols;
    char *string_table;
    void *load_address;
    size_t total_size;
    uint64_t entry_address;
} mwp_program_t;

mwp_program_t* mwp_load(const char *path);
int mwp_execute(mwp_program_t *prog);
void mwp_unload(mwp_program_t *prog);
void mwp_register_symbol(const char *name, void *addr);
void mwp_init_exports(void);

#endif
