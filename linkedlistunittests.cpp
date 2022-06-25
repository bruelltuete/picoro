#include "linkedlist.h"

#if !PICO_PRINTF_ALWAYS_INCLUDED
// if the above symbol is not defined then assert's printf does not work!
#endif
// copied from assert macro.
#define CHECK(__e) ((__e) ? (void)0 : __assert_func(__FILE__, __LINE__, __PRETTY_FUNCTION__, #__e))


struct UnitTestListEntry
{
    uint32_t    someothervalue;
    struct LinkedListEntry  listentry;
};

extern "C" void ll_unit_test()
{
    struct LinkedList   list;
    ll_init_list(&list);


    // is_empty

    bool empty = ll_is_empty(&list);
    CHECK(empty);


    // push_back

    struct UnitTestListEntry value1;
    value1.someothervalue = 111111111;
    ll_push_back(&list, &value1.listentry);
    empty = ll_is_empty(&list);
    CHECK(!empty);

    struct UnitTestListEntry value2;
    value2.someothervalue = 222222222;
    ll_push_back(&list, &value2.listentry);


    // peek_tail

    struct LinkedListEntry* tail = ll_peek_tail(&list);
    CHECK(&value2.listentry == tail);


    // peek_head

    struct LinkedListEntry* head = ll_peek_head(&list);
    CHECK(&value1.listentry == head);


    // pop_front

    struct LinkedListEntry* front1 = ll_pop_front(&list);
    CHECK(&value1.listentry == front1);
    CHECK(value1.someothervalue == LL_ACCESS(&value1, listentry, front1)->someothervalue);

    empty = ll_is_empty(&list);
    CHECK(!empty);

    struct LinkedListEntry* front2 = ll_pop_front(&list);
    CHECK(&value2.listentry == front2);
    CHECK(value2.someothervalue == LL_ACCESS(&value2, listentry, front2)->someothervalue);
    empty = ll_is_empty(&list);
    CHECK(empty);

    struct LinkedListEntry* front_na = ll_pop_front(&list);
    CHECK(NULL == front_na);
    empty = ll_is_empty(&list);
    CHECK(empty);


    // push_front

    struct UnitTestListEntry value3;
    value3.someothervalue = 333333333;
    ll_push_front(&list, &value3.listentry);

    struct UnitTestListEntry value4;
    value4.someothervalue = 444444444;
    ll_push_front(&list, &value4.listentry);

    struct LinkedListEntry* front4 = ll_pop_front(&list);
    CHECK(&value4.listentry == front4);

    struct LinkedListEntry* front3 = ll_pop_front(&list);
    CHECK(&value3.listentry == front3);


    // ll_remove

    CHECK(ll_is_empty(&list));
    ll_push_back(&list, &value1.listentry);
    ll_remove(&list, &value1.listentry);
    CHECK(ll_is_empty(&list));

    ll_push_back(&list, &value1.listentry);
    ll_push_back(&list, &value2.listentry);
    ll_remove(&list, &value1.listentry);        // remove head
    CHECK(&value2.listentry == ll_peek_head(&list));

    ll_push_back(&list, &value1.listentry);
    ll_remove(&list, &value1.listentry);        // remove tail
    CHECK(&value2.listentry == ll_peek_tail(&list));

    ll_push_back(&list, &value3.listentry);
    ll_push_back(&list, &value1.listentry);     // order is: v2, v3, v1
    ll_remove(&list, &value3.listentry);        // remove in the middle
    CHECK(&value2.listentry == ll_peek_head(&list));
    CHECK(&value1.listentry == ll_peek_tail(&list));


    // ll_sorted_insert

    ll_pop_front(&list);    // empty list
    ll_pop_front(&list);
    CHECK(ll_is_empty(&list));
    ll_sorted_insert<(int) offsetof(UnitTestListEntry, someothervalue) - (int) offsetof(UnitTestListEntry, listentry), uint32_t>(&list, &value2.listentry);
    CHECK(&value2.listentry == ll_peek_head(&list));

    ll_sorted_insert<(int) offsetof(UnitTestListEntry, someothervalue) - (int) offsetof(UnitTestListEntry, listentry), uint32_t>(&list, &value3.listentry);
    CHECK(&value2.listentry == ll_peek_head(&list));
    CHECK(&value3.listentry == ll_peek_tail(&list));

    ll_sorted_insert<(int) offsetof(UnitTestListEntry, someothervalue) - (int) offsetof(UnitTestListEntry, listentry), uint32_t>(&list, &value1.listentry);
    CHECK(&value1.listentry == ll_peek_head(&list));
    CHECK(&value3.listentry == ll_peek_tail(&list));

    ll_sorted_insert<(int) offsetof(UnitTestListEntry, someothervalue) - (int) offsetof(UnitTestListEntry, listentry), uint32_t>(&list, &value4.listentry);
    CHECK(&value1.listentry == ll_peek_head(&list));
    CHECK(&value4.listentry == ll_peek_tail(&list));
}
