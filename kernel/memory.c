#include "memory.h"
#include "serial.h"
#include <stdbool.h>
#include <stdint.h>

// =======================
// Config
// =======================

#define HEAP_BASE_ADDR 0x10000000
#define HEAP_SIZE (256 * 1024 * 1024)

#define HEAP_ALIGN 8
#define HEAP_MIN_UNIT 128

#define BLOCK_MAGIC 0xC0FFEE01u
#define CANARY_VALUE 0xBADC0DEu

#define BLOCK_FREE 0x1u

// #define DEBUG

typedef struct block_header {
    uint32_t magic;
    uint32_t size; // user size
    uint32_t flags;
    struct block_header* next;
    struct block_header* prev;
} block_header_t;

static uint8_t* heap_base = (uint8_t*)HEAP_BASE_ADDR;
static uint32_t heap_total_size = HEAP_SIZE;

static block_header_t* free_list_head = NULL;

// =======================
// Helpers
// =======================

static inline uint32_t align_up(uint32_t x, uint32_t align) {
    return (x + align - 1) & ~(align - 1);
}

static inline uint8_t* block_user_ptr(block_header_t* blk) {
    uint8_t* p = (uint8_t*)(blk + 1);
    p += sizeof(uint32_t);
    return p;
}

static inline block_header_t* user_to_block(void* ptr) {
    if (!ptr)
        return NULL;
    uint8_t* p = (uint8_t*)ptr;
    p -= sizeof(uint32_t);
    return ((block_header_t*)p) - 1;
}

static void block_write_canaries(block_header_t* blk) {
    uint8_t* user = block_user_ptr(blk);
    uint32_t* head = (uint32_t*)(user - sizeof(uint32_t));
    uint32_t* tail = (uint32_t*)(user + blk->size);
    *head = CANARY_VALUE;
    *tail = CANARY_VALUE;
}

static bool block_check_canaries(block_header_t* blk) {
    uint8_t* user = block_user_ptr(blk);
    uint32_t* head = (uint32_t*)(user - sizeof(uint32_t));
    uint32_t* tail = (uint32_t*)(user + blk->size);

    bool ok = true;

    if (*head != CANARY_VALUE) {
        serial_puts("[ERROR] kmalloc: head canary corrupted at block ");
        serial_puthex64((uint64_t)blk);
        serial_puts("\n");
        ok = false;
    }
    if (*tail != CANARY_VALUE) {
        serial_puts("[ERROR] kmalloc: tail canary corrupted at block ");
        serial_puthex64((uint64_t)blk);
        serial_puts("\n");
        ok = false;
    }
    return ok;
}

// =======================
// Free list ops
// =======================

static void free_list_insert(block_header_t* blk) {
    blk->flags |= BLOCK_FREE;
    blk->prev = NULL;
    blk->next = free_list_head;
    if (free_list_head)
        free_list_head->prev = blk;
    free_list_head = blk;
}

static void free_list_remove(block_header_t* blk) {
    if (!(blk->flags & BLOCK_FREE))
        return;

    if (blk->prev)
        blk->prev->next = blk->next;
    else
        free_list_head = blk->next;

    if (blk->next)
        blk->next->prev = blk->prev;

    blk->flags &= ~BLOCK_FREE;
    blk->next = blk->prev = NULL;
}

// =======================
// Coalescing
// =======================

static void try_coalesce_with_next(block_header_t* blk) {
    uint32_t used = sizeof(block_header_t) + sizeof(uint32_t) + blk->size +
                    sizeof(uint32_t);
    uint8_t* next_addr = (uint8_t*)blk + used;

    if ((uintptr_t)next_addr >= (uintptr_t)(heap_base + heap_total_size))
        return;

    block_header_t* next_blk = (block_header_t*)next_addr;

    if (next_blk->magic != BLOCK_MAGIC)
        return;
    if (!(next_blk->flags & BLOCK_FREE))
        return;

#ifdef DEBUG
    serial_puts("[INFO]  kfree: coalescing with next block at ");
    serial_puthex64((uint64_t)blk);
    serial_puts("\n");
#endif

    free_list_remove(next_blk);

    uint32_t next_used = sizeof(block_header_t) + sizeof(uint32_t) +
                         next_blk->size + sizeof(uint32_t);
    blk->size += next_used;

    block_write_canaries(blk);
}

// =======================
// Public API
// =======================

void mem_init(void) {
    heap_base = (uint8_t*)HEAP_BASE_ADDR;
    heap_total_size = HEAP_SIZE;

    block_header_t* first = (block_header_t*)heap_base;
    first->magic = BLOCK_MAGIC;

    uint32_t usable = heap_total_size - sizeof(block_header_t) -
                      sizeof(uint32_t) - sizeof(uint32_t);
    first->size = usable;
    first->flags = 0;
    first->next = NULL;
    first->prev = NULL;

    free_list_head = NULL;
    free_list_insert(first);
    block_write_canaries(first);

#ifdef DEBUG
    serial_puts("[INFO]  Memory manager initialized at ");
    serial_puthex64((uint64_t)HEAP_BASE_ADDR);
    serial_puts(" (");
    serial_putdec64(HEAP_SIZE / 1024 / 1024);
    serial_puts(" MB heap)\n");
#endif
}

void* kmalloc(uint32_t size) {
    if (size == 0)
        return NULL;

    uint32_t aligned = align_up(size, HEAP_ALIGN);
    if (aligned < HEAP_MIN_UNIT)
        aligned = HEAP_MIN_UNIT;

    uint32_t needed =
        sizeof(block_header_t) + sizeof(uint32_t) + aligned + sizeof(uint32_t);

    // best-fit search
    block_header_t* best = NULL;
    uint32_t best_total = 0;

    for (block_header_t* cur = free_list_head; cur; cur = cur->next) {
        if (!(cur->flags & BLOCK_FREE))
            continue;

        uint32_t cur_total = sizeof(block_header_t) + sizeof(uint32_t) +
                             cur->size + sizeof(uint32_t);
        if (cur_total < needed)
            continue;

        if (!best || cur_total < best_total) {
            best = cur;
            best_total = cur_total;
        }
    }

    if (!best) {
        serial_puts("[ERROR] kmalloc failed: out of memory for size=");
        serial_putdec64(size);
        serial_puts("\n");
        return NULL;
    }

    uint32_t min_remain_total = sizeof(block_header_t) + sizeof(uint32_t) +
                                HEAP_MIN_UNIT + sizeof(uint32_t);

    if (best_total >= needed + min_remain_total) {
        // split
        uint8_t* addr = (uint8_t*)best;

        block_header_t* new_blk = best;
        block_header_t* remain_blk = (block_header_t*)(addr + needed);

        remain_blk->magic = BLOCK_MAGIC;
        uint32_t remain_total = best_total - needed;
        uint32_t remain_usable = remain_total - sizeof(block_header_t) -
                                 sizeof(uint32_t) - sizeof(uint32_t);

        remain_blk->size = remain_usable;
        remain_blk->flags = 0;
        remain_blk->next = remain_blk->prev = NULL;
        block_write_canaries(remain_blk);

        free_list_remove(best);
        free_list_insert(remain_blk);

        new_blk->size = aligned;
        new_blk->flags = 0;
        new_blk->next = new_blk->prev = NULL;
        block_write_canaries(new_blk);

        uint8_t* user = block_user_ptr(new_blk);

#ifdef DEBUG
        serial_puts("[INFO]  kmalloc: allocated ");
        serial_putdec64(size);
        serial_puts(" bytes (aligned ");
        serial_putdec64(aligned);
        serial_puts(") at ");
        serial_puthex64((uint64_t)user);
        serial_puts(" (split)\n");
#endif

        return user;
    } else {
        // take whole block
        free_list_remove(best);
        best->size = aligned;
        best->flags &= ~BLOCK_FREE;
        block_write_canaries(best);

        uint8_t* user = block_user_ptr(best);

#ifdef DEBUG
        serial_puts("[INFO]  kmalloc: allocated ");
        serial_putdec64(size);
        serial_puts(" bytes (aligned ");
        serial_putdec64(aligned);
        serial_puts(") at ");
        serial_puthex64((uint64_t)user);
        serial_puts(" (whole block)\n");
#endif

        return user;
    }
}

void kfree(void* ptr) {
    if (!ptr)
        return;

    block_header_t* blk = user_to_block(ptr);
    if (!blk || blk->magic != BLOCK_MAGIC) {
        serial_puts("[ERROR] kfree: invalid pointer ");
        serial_puthex64((uint64_t)ptr);
        serial_puts("\n");
        return;
    }

    if (blk->flags & BLOCK_FREE) {
        serial_puts("[ERROR] kfree: double free detected at ");
        serial_puthex64((uint64_t)ptr);
        serial_puts("\n");
        return;
    }

    block_check_canaries(blk);

#ifdef DEBUG
    serial_puts("[INFO]  kfree: freed ");
    serial_putdec64(blk->size);
    serial_puts(" bytes at ");
    serial_puthex64((uint64_t)ptr);
    serial_puts("\n");
#endif

    // clear user memory
    uint8_t* user = (uint8_t*)ptr;
    for (uint32_t i = 0; i < blk->size; i++)
        user[i] = 0;

    free_list_insert(blk);
    try_coalesce_with_next(blk);
}
