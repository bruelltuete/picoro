
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
        return NULL;

    struct LinkedListEntry* newhead = value->next;
    list->head = newhead;
    if (newhead == NULL)
        list->tail = NULL;

    return value;
}

#define LL_ACCESS_INTERNAL(type, given, offset)      ((type) (((uint8_t*) (given)) + offset))
#define LL_ACCESS(enclosingstructptr, listentrymembername, ptr)   LL_ACCESS_INTERNAL(typeof(enclosingstructptr), ptr, -offsetof(typeof(*enclosingstructptr), listentrymembername))


struct UnitTestListEntry
{
    uint32_t    someothervalue;
    struct LinkedListEntry  listentry;
};

static inline void ll_unit_test()
{
    struct LinkedList   list;
    ll_init_list(&list);


    // is_empty

    bool empty = ll_is_empty(&list);
    assert(empty);


    // push_back

    struct UnitTestListEntry value1;
    value1.someothervalue = 1234567890;
    ll_push_back(&list, &value1.listentry);
    empty = ll_is_empty(&list);
    assert(!empty);

    struct UnitTestListEntry value2;
    value2.someothervalue = 987654321;
    ll_push_back(&list, &value2.listentry);


    // peek_tail

    struct LinkedListEntry* tail = ll_peek_tail(&list);
    assert(&value2.listentry == tail);


    // peek_head

    struct LinkedListEntry* head = ll_peek_head(&list);
    assert(&value1.listentry == head);


    // pop_front

    struct LinkedListEntry* front1 = ll_pop_front(&list);
    assert(&value1.listentry == front1);
    assert(value1.someothervalue == LL_ACCESS(&value1, listentry, front1)->someothervalue);

    empty = ll_is_empty(&list);
    assert(!empty);

    struct LinkedListEntry* front2 = ll_pop_front(&list);
    assert(&value2.listentry == front2);
    assert(value2.someothervalue == LL_ACCESS(&value2, listentry, front2)->someothervalue);
    empty = ll_is_empty(&list);
    assert(empty);

    struct LinkedListEntry* front_na = ll_pop_front(&list);
    assert(NULL == front_na);
    empty = ll_is_empty(&list);
    assert(empty);


    // push_front

    struct UnitTestListEntry value3;
    ll_push_front(&list, &value3.listentry);

    struct UnitTestListEntry value4;
    ll_push_front(&list, &value4.listentry);

    struct LinkedListEntry* front4 = ll_pop_front(&list);
    assert(&value4.listentry == front4);

    struct LinkedListEntry* front3 = ll_pop_front(&list);
    assert(&value3.listentry == front3);
}