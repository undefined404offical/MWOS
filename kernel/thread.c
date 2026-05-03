// thread.c
#include "thread.h"
#include "memory.h"
#include "serial.h"
#include <string.h>

// 当前正在运行的线程
static thread_t *current = NULL;
// 仅用于调试打印的全局链表（不参与调度）
static thread_t *thread_list = NULL;

#define THREAD_STACK_SIZE (16 * 1024)
#define MAX_PRIORITY 31
#define NR_QUEUES 32

static void thread_trampoline(void);

typedef struct runqueue
{
    thread_t *head[NR_QUEUES];
    thread_t *tail[NR_QUEUES];
    uint32_t bitmap; // 哪些优先级队列非空
} runqueue_t;

static runqueue_t rq;

#define THR_BLOCK_MIN_SIZE (16 * 1024) // 每块至少 16KB
#define THR_BLOCK_ALIGN 16
#define THR_HEAP_MAX_TOTAL (16 * 1024 * 1024) // 每线程最多 4MB，可自己调

static inline size_t align_up(size_t x, size_t a)
{
    return (x + a - 1) & ~(a - 1);
}

static inline thread_t *current_thread(void)
{
    return current;
}

static thr_block_t *thr_block_new(size_t min_size)
{
    size_t data_size = min_size;
    if (data_size < THR_BLOCK_MIN_SIZE)
        data_size = THR_BLOCK_MIN_SIZE;

    size_t total = sizeof(thr_block_t) + data_size;
    thr_block_t *blk = (thr_block_t *)kmalloc(total);
    if (!blk)
        return NULL;

    blk->next = NULL;
    blk->size = data_size;
    blk->used = 0;
    return blk;
}

static size_t thr_heap_total_size(thr_heap_t *heap)
{
    size_t sum = 0;
    for (thr_block_t *b = heap->head; b; b = b->next)
    {
        sum += b->size;
    }
    return sum;
}

void *thr_alloc(size_t size)
{
    if (size == 0)
        return NULL;

    thread_t *t = current_thread();
    if (!t)
        return NULL;

    thr_heap_t *heap = &t->heap;
    size = align_up(size, THR_BLOCK_ALIGN);

    size_t cur_total = thr_heap_total_size(heap);
    if (cur_total + size > THR_HEAP_MAX_TOTAL)
    {
        serial_puts("thr_alloc: heap overflow, killing thread\n");
        t->state = THREAD_FINISHED;
        while (1)
            asm volatile("hlt");
    }

    thr_block_t *blk = heap->head;

    if (!blk)
    {
        blk = thr_block_new(size);
        if (!blk)
            return NULL;
        heap->head = blk;
    }

    if (blk->used + size > blk->size)
    {
        thr_block_t *new_blk = thr_block_new(size);
        if (!new_blk)
            return NULL;
        new_blk->next = heap->head;
        heap->head = new_blk;
        blk = new_blk;
    }

    void *ptr = blk->data + blk->used;
    blk->used += size;
    return ptr;
}

void thr_free_all(void)
{
    thread_t *t = current_thread();
    if (!t)
        return;

    thr_heap_t *heap = &t->heap;
    thr_block_t *blk = heap->head;
    while (blk)
    {
        thr_block_t *next = blk->next;
        kfree(blk);
        blk = next;
    }
    heap->head = NULL;
}

// ---------------- runqueue 操作 ----------------

static void runqueue_init(void)
{
    memset(&rq, 0, sizeof(rq));
}

static void runqueue_push(thread_t *t)
{
    int prio = t->priority;
    if (prio < 0)
        prio = 0;
    if (prio > MAX_PRIORITY)
        prio = MAX_PRIORITY;

    t->next = NULL;

    if (!rq.head[prio])
    {
        rq.head[prio] = rq.tail[prio] = t;
        rq.bitmap |= (1u << prio);
    }
    else
    {
        rq.tail[prio]->next = t;
        rq.tail[prio] = t;
    }
}

static thread_t *runqueue_pop_highest(void)
{
    if (!rq.bitmap)
        return NULL;

    // 找最高优先级（最高位的 1）
    int prio = 31 - __builtin_clz(rq.bitmap);

    thread_t *t = rq.head[prio];
    if (!t)
    {
        // 理论上不该发生，防御一下
        rq.bitmap &= ~(1u << prio);
        return NULL;
    }

    rq.head[prio] = t->next;
    if (!rq.head[prio])
    {
        rq.tail[prio] = NULL;
        rq.bitmap &= ~(1u << prio);
    }

    t->next = NULL;
    return t;
}

// ---------------- 调试用链表 ----------------

static void thread_append(thread_t *t)
{
    t->next = NULL;
    if (!thread_list)
    {
        thread_list = t;
    }
    else
    {
        thread_t *cur = thread_list;
        while (cur->next)
            cur = cur->next;
        cur->next = t;
    }

    serial_puts("thread_list:");
    thread_t *p = thread_list;
    while (p)
    {
        serial_puts(" -> ");
        serial_puthex64((uint64_t)p);
        serial_puts(" [state=");
        serial_putdec64(p->state);
        serial_puts(" prio=");
        serial_putdec64(p->priority);
        serial_puts("]");
        p = p->next;
    }
    serial_puts("\n");
}

// ---------------- 调度器接口 ----------------

void scheduler_init(void)
{
    current = NULL;
    thread_list = NULL;
    runqueue_init();
}

thread_t *thread_create(void (*entry)(void *), void *arg, int priority)
{
    thread_t *t = (thread_t *)kmalloc(sizeof(thread_t));
    if (!t)
        return NULL;

    uint8_t *stack = (uint8_t *)kmalloc(THREAD_STACK_SIZE);
    if (!stack)
        return NULL;

    t->stack_base = stack;
    t->stack_size = THREAD_STACK_SIZE;
    t->state = THREAD_READY;
    t->entry = entry;
    t->arg = arg;
    t->priority = priority;

    // 对齐栈顶
    uint64_t stack_top = (uint64_t)(stack + THREAD_STACK_SIZE);
    stack_top &= ~0xFULL;

    uint64_t *sp = (uint64_t *)stack_top;
    *(--sp) = 0; // fake return address

    memset(&t->tf, 0, sizeof(Trapframe));
    t->tf.rip = (uint64_t)thread_trampoline;
    t->tf.cs = 0x08;
    t->tf.ss = 0x10;
    t->tf.rflags = 0x202;
    t->tf.rsp = (uint64_t)sp;

    t->tf.rax = 0;
    t->tf.rbx = 0;
    t->tf.rcx = 0;
    t->tf.rdx = 0;
    t->tf.rsi = 0;
    t->tf.rdi = 0;
    t->tf.rbp = 0;
    t->tf.r8 = 0;
    t->tf.r9 = 0;
    t->tf.r10 = 0;
    t->tf.r11 = 0;
    t->tf.r12 = 0;
    t->tf.r13 = 0;
    t->tf.r14 = 0;
    t->tf.r15 = 0;
    t->heap.head = NULL;
    // 调试链表
    thread_append(t);
    // 放入就绪队列
    runqueue_push(t);

    serial_puts("thread: created thread at ");
    serial_puthex64((uint64_t)(uintptr_t)t);
    serial_puts(" prio=");
    serial_putdec64(t->priority);
    serial_puts(" stack=0x");
    serial_puthex64((uint64_t)(uintptr_t)stack);
    serial_puts("\n");

    return t;
}

// 纯 Trapframe + 抢占式调度下，线程内部不主动让出 CPU
void thread_yield(void)
{
    // no-op
}

void scheduler_start(void)
{
    // 从就绪队列里取第一个线程
    current = runqueue_pop_highest();
    if (!current)
    {
        serial_puts("scheduler_start: no runnable threads\n");
        while (1)
            asm volatile("hlt");
    }

    current->state = THREAD_RUNNING;

    // 在当前内核栈上直接跑第一个线程
    thread_trampoline();

    while (1)
    {
        asm volatile("hlt");
    }
}

static void thread_trampoline(void)
{
    thread_t *t = current;
    t->entry(t->arg);

    serial_puts("thread: finished ");
    serial_puthex64((uint64_t)(uintptr_t)t);
    serial_puts("\n");

    t->state = THREAD_FINISHED;
    thr_free_all();

    // 线程结束后不再回到调度器，由 tick 驱动其他线程继续跑
    while (1)
    {
        asm volatile("hlt");
    }
}

void scheduler_tick(Trapframe *tf)
{
    if (!current)
        return;

    // 保存当前线程现场
    current->tf = *tf;

    // 如果当前线程还在跑且没结束，就放回就绪队列
    if (current->state == THREAD_RUNNING)
    {
        current->state = THREAD_READY;
        runqueue_push(current);
    }

    // 取最高优先级的下一个线程
    thread_t *next = runqueue_pop_highest();
    if (!next)
    {
        // 没有可运行线程，就继续当前（可能是 idle）
        return;
    }

    next->state = THREAD_RUNNING;
    current = next;

    // 恢复下一个线程现场
    *tf = current->tf;
}

void idle_thread(void *arg)
{
    (void)arg;
    for (;;)
    {
        asm volatile("hlt");
    }
}
