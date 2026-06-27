#ifndef LIST_H
#define LIST_H

#include <stddef.h>
#include <stdbool.h>

// 链表节点定义
typedef struct node {
    void *data;             // 通用数据指针
    struct node *next;      // 指向下一个节点的指针
} node;

// 链表容器定义
typedef struct list {
    node *head;
    node *tail;
    size_t size;
    void (*free_data)(void *); // 用于释放数据内存的回调函数
} list;

// 函数接口
list* list_create();
void list_destroy(list *list);
bool list_push_back(list *list, void *data);
bool list_push_front(list *list, void *data);
void* list_pop_front(list *list);
size_t list_size(const list *list);
bool list_insert(list *l, size_t index, void *data);
bool list_remove(list *l, void *data);
void* list_get(list *l, size_t index);
int list_find(list *l, void *data);

#endif