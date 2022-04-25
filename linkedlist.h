
#pragma once
#include "pico/stdlib.h"

struct LinkedListEntry
{
    struct LinkedListEntry*    next;
};

/**
 * @brief Minimal intrusive singly-linked-list (forward only).
 * Does not allocate memory, you need to embed LinkedListEntry into your structure.
 */
struct LinkedList
{
    struct LinkedListEntry*    head;
    struct LinkedListEntry*    tail;
};

static inline void ll_init_list(struct LinkedList* list)
{
    list->head = NULL;
    list->tail = NULL;
}

static inline bool ll_is_empty(struct LinkedList* list)
{
    if (list->head == NULL)
        return true;
    // if not empty, ie we have a head then we should also have a tail
    assert(list->tail != NULL);
    return false;
}

static inline struct LinkedListEntry* ll_peek_tail(struct LinkedList* list)
{
    if (list->tail == NULL)
    {
        assert(list->head == NULL);
        return NULL;
    }

    return list->tail;
}

static inline struct LinkedListEntry* ll_peek_head(struct LinkedList* list)
{
    if (list->head == NULL)
    {
        assert(list->tail == NULL);
        return NULL;
    }

    return list->head;
}

static inline void ll_push_back(struct LinkedList* list, struct LinkedListEntry* value)
{
    // catch easy mistake: a value can only be in one list at a time, there's only 1 intrusive link!
    assert(value != list->head);
    assert(value != list->tail);

    if (list->head == NULL)
    {
        assert(list->tail == NULL);
        list->head = list->tail = value;
        value->next = NULL;
        return;
    }

    assert(list->tail != NULL);
    list->tail->next = value;
    list->tail = value;
    value->next = NULL;
}

static inline void ll_push_front(struct LinkedList* list, struct LinkedListEntry* value)
{
    // catch easy mistake: a value can only be in one list at a time, there's only 1 intrusive link!
    assert(value != list->head);
    assert(value != list->tail);

    if (list->head == NULL)
    {
        assert(list->tail == NULL);
        list->head = list->tail = value;
        value->next = NULL;
        return;
    }

    assert(list->tail != NULL);
    value->next = list->head;
    list->head = value;
}

static inline struct LinkedListEntry* ll_pop_front(struct LinkedList* list)
{
    struct LinkedListEntry* value = list->head;
    if (value == NULL)
    {
        // empty list
        assert(list->tail == NULL);
        return NULL;
    }

    struct LinkedListEntry* newhead = value->next;
    list->head = newhead;
    if (newhead == NULL)
        list->tail = NULL;

#ifndef NDEBUG
    value->next = (LinkedListEntry*) 0xdeadbeef;
#endif

    return value;
}

/**
 * @brief Removes the given value from the list by scanning for it from the head towards the tail until found.
 * If value is not contained in the list then nothing happens, apart from wasting time scanning through the list.
 * @param list 
 * @param value 
 */
static inline void ll_remove(struct LinkedList* list, struct LinkedListEntry* value)
{
    for (struct LinkedListEntry* i = list->head, *p = NULL; i != NULL; p = i, i = i->next)
    {
        if (i == value)
        {
            if (i == list->head)
                list->head = i->next;
            if (i == list->tail)
                list->tail = p;
            if (p != NULL)
            {
                assert(p->next == i);
                p->next = i->next;
            }

#ifndef NDEBUG
            i->next = (LinkedListEntry*) 0xdeadbeef;
#endif
            return;
        }
    }
}

template <typename T>
T LL_ACCESS_INTERNAL(void* given, int offset)
{
    if (given == NULL)
        return NULL;
    return ((T) (((uint8_t*) (given)) + offset));
}

// value is inserted at a position if its comparison says it's lower. otherwise after.
template <int offsetFromListEntry = -8, typename T = uint64_t>
static inline void ll_sorted_insert(struct LinkedList* list, struct LinkedListEntry* value)
{
    // shortcut
    if (list->head == NULL)
    {
        assert(list->tail == NULL);
        list->head = list->tail = value;
        value->next = NULL;
        return;
    }
        
    const T& vb = *LL_ACCESS_INTERNAL<T*>(value, offsetFromListEntry);

    for (struct LinkedListEntry* i = list->head, *p = NULL; i != NULL; p = i, i = i->next)
    {
        const T& va = *LL_ACCESS_INTERNAL<T*>(i, offsetFromListEntry);
        if (vb <= va)
        {
            // insert before the current node i
            if (p == NULL)
                list->head = value;
            else
                p->next = value;
            value->next = i;
            return;
        }
    }

    list->tail->next = value;
    value->next = NULL;
    list->tail = value;
}

#define LL_ACCESS(enclosingstructptr, listentrymembername, ptr)   LL_ACCESS_INTERNAL<typeof(enclosingstructptr)>(ptr, -offsetof(typeof(*enclosingstructptr), listentrymembername))

extern "C" void ll_unit_test();
