#include "cstd.h"
#include "list.h"
#include "memory.h"

// 创建链表
list *list_create()
{
    list *l = (list *)kmalloc(sizeof(list));
    if (!l)
        return NULL;
    l->head = NULL;
    l->tail = NULL;
    l->size = 0;
    l->free_data = kfree;
    return l;
}

// 尾部插入
bool list_push_back(list *l, void *data)
{
    node *new_node = (node *)kmalloc(sizeof(node));
    if (!new_node)
        return false;

    new_node->data = data;
    new_node->next = NULL;

    if (l->size == 0)
    {
        l->head = l->tail = new_node;
    }
    else
    {
        l->tail->next = new_node;
        l->tail = new_node;
    }
    l->size++;
    return true;
}

// 头部删除并返回数据
void *list_pop_front(list *l)
{
    if (l->size == 0)
        return NULL;

    node *old_head = l->head;
    void *data = old_head->data;

    l->head = old_head->next;
    if (l->head == NULL)
    {
        l->tail = NULL;
    }

    kfree(old_head);
    l->size--;
    return data;
}

// 销毁链表并释放所有节点和数据
void list_destroy(list *l)
{
    node *current = l->head;
    while (current)
    {
        node *next = current->next;
        if (l->free_data && current->data)
        {
            l->free_data(current->data); // 调用回调释放用户数据
        }
        kfree(current);
        current = next;
    }
    kfree(l);
}

// 获取链表长度
size_t list_size(const list *l)
{
    return l->size;
}

// 在链表中插入元素
bool list_insert(list *l, size_t index, void *data)
{
    if (index > l->size)
        return false;

    // 如果插入位置是头部
    if (index == 0)
    {
        node *new_node = (node *)kmalloc(sizeof(node));
        if (!new_node)
            return false;
        new_node->data = data;
        new_node->next = l->head;
        l->head = new_node;
        if (l->size == 0)
            l->tail = new_node;
        l->size++;
        return true;
    }

    // 如果插入位置是尾部，直接复用已有的 push_back
    if (index == l->size)
    {
        return list_push_back(l, data);
    }

    // 中间插入：找到目标位置的前驱节点
    node *new_node = (node *)kmalloc(sizeof(node));
    if (!new_node)
        return false;
    new_node->data = data;

    node *prev = l->head;
    for (size_t i = 0; i < index - 1; i++)
    {
        prev = prev->next;
    }

    new_node->next = prev->next;
    prev->next = new_node;
    l->size++;
    return true;
}

// 删除链表中的指定元素
bool list_remove(list *l, void *data)
{
    if (l->size == 0)
        return false;

    node *current = l->head;
    node *prev = NULL;

    while (current)
    {
        if (current->data == data)
        {
            // 找到目标节点
            if (prev == NULL)
            {
                // 情况1：删除的是头节点
                l->head = current->next;
                if (l->head == NULL)
                    l->tail = NULL;
            }
            else
            {
                // 情况2：删除的是中间或尾部节点
                prev->next = current->next;
                if (current == l->tail)
                    l->tail = prev;
            }

            // 释放数据内存（如果注册了回调）
            if (l->free_data && current->data)
            {
                l->free_data(current->data);
            }

            kfree(current);
            l->size--;
            return true;
        }
        prev = current;
        current = current->next;
    }

    return false; // 未找到匹配数据
}

// 查看指定索引项的数据
void *list_get(list *l, size_t index)
{
    if (index >= l->size)
        return NULL;

    node *current = l->head;
    for (size_t i = 0; i < index; i++)
    {
        current = current->next;
    }

    return current->data;
}

// 查找指定数据在链表中的索引（返回 -1 表示未找到）
int list_find(list *l, void *data)
{
    node *current = l->head;
    int index = 0;

    while (current)
    {
        if (current->data == data)
        {
            return index;
        }
        current = current->next;
        index++;
    }

    return -1;
}
