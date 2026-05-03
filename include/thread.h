#ifndef THREAD_H
#define THREAD_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "trap.h"

typedef enum {
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_FINISHED
} thread_state_t;

typedef struct thr_block {
    struct thr_block* next;
    size_t            size;   // data 区大小
    size_t            used;   // 已用
    uint8_t           data[]; // 小块分配区域
} thr_block_t;

typedef struct {
    thr_block_t* head;
} thr_heap_t;

typedef struct thread {
    uint8_t* stack_base;
    uint64_t stack_size;

    void (*entry)(void*);
    void* arg;
    int priority;
    Trapframe tf;
    thread_state_t state;

    struct thread* next;

    thr_heap_t heap;
} thread_t;

void thread_init();
thread_t* thread_create(void (*entry)(void*), void* arg, int priority);
void scheduler_start();
void scheduler_tick(Trapframe* tf);
void thread_yield();
void idle_thread(void* arg);

#endif
