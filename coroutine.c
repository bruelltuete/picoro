#include "coroutine.h"
#include "pico/stdlib.h"
#include <string.h>


//struct Coroutine*   waiting4dma_head;
//struct Coroutine*   waiting4timer_head;


// tail is currently running.
// head is the one up next.
struct LinkedList   ready2run;


struct CoroutineHeader
{
    volatile uint32_t*      sp;
    struct LinkedListEntry  llentry;
}  mainflow;


void yield_and_exit()
{
}

// returns stack pointer for next coro
static volatile uint32_t* schedule_next(volatile uint32_t* current_sp)
{
    if (ll_is_empty(&ready2run))
        return mainflow.sp;

    struct Coroutine* currentcoro = LL_ACCESS(currentcoro, llentry, ll_peek_tail(&ready2run));
    currentcoro->sp = current_sp;

    struct Coroutine* upnext = LL_ACCESS(upnext, llentry, ll_pop_front(&ready2run));
    ll_push_back(&ready2run, &upnext->llentry);

    return upnext->sp;
}

void __attribute__ ((naked)) yield()
{
    __asm volatile (
        // old stack is still active
        "push {lr};"
        "push {r0, r1, r2, r3, r4, r5, r6, r7};"
        // push only has encoding up to r7, so to push the other registers we need to copy them to r0-7 first.
        "mov r1, r8;"
        "mov r2, r9;"
        "mov r3, r10;"
        "mov r4, r11;"
        "mov r5, r12;"
        // r13 = stack pointer, r14 = link register, r15 = pc.
        "push {r1, r2, r3, r4, r5};"
        // insight: we do not need to preserve pc! execution will always continue from here on (just with a different stack).

        "mov r0, sp;"
        "bl schedule_next;"
        "mov sp, r0;"

        // restore all those registers with the new stack.
        "pop {r1, r2, r3, r4, r5};"
        "mov r8, r1;"
        "mov r9, r2;"
        "mov r10, r3;"
        "mov r11, r4;"
        "mov r12, r5;"
        "pop {r0, r1, r2, r3, r4, r5, r6, r7};"
        "pop {pc};"     // return to caller, straight-forward box-standard nothing-fancy return.
        : // out
        : // in
        : "memory"  // clobber: make sure compiler has generated stores before and loads after this block.
    );

    // will not get here
    while (true)
        __breakpoint();
}

// One extra step for calling coro's entry point to make sure there's a yield_and_exit() when it returns.
static void entry_point_wrapper(coroutinefp_t func, int param)
{
    func(param);

    // will never return here!
    yield_and_exit();

    // but if we do by accident somehow...
    while (true)
        __breakpoint();
}

void yield_and_start(coroutinefp_t func, int param, struct Coroutine* storage)
{
    // for debugging
    memset((void*) &storage->stack[0], 0, sizeof(storage->stack));

    const int bottom_element = sizeof(storage->stack) / sizeof(storage->stack[0]);
    // points to past the last element!
    storage->sp = &storage->stack[bottom_element];
    // "push" some values onto the stack.
    // this needs to match what yield() does!
    *--storage->sp = entry_point_wrapper;
    *--storage->sp = 0;
    *--storage->sp = 0;
    *--storage->sp = 0;
    *--storage->sp = 0;
    *--storage->sp = 0;
    *--storage->sp = 0;
    *--storage->sp = param;     // r1
    *--storage->sp = func;      // r0
    *--storage->sp = 0;
    *--storage->sp = 0;
    *--storage->sp = 0;
    *--storage->sp = 0;
    *--storage->sp = 0;
/*
        "push {lr};"
        "push {r0, r1, r2, r3, r4, r5, r6, r7};"
        "push {r8, r9, r10, r11, r12};"
*/

    if (!mainflow.sp)
        ll_init_list(&ready2run);

    // FIXME: should this be push_front()? this function call here is yield_and_start(), but we are only yielding, starting is happening much later. which is ok?
    //        but but but: tail is the currently executing coro!
    ll_push_front(&ready2run, &storage->llentry);

    if (!mainflow.sp)
    {
        // i need mainflow in ready2run.tail so that schedule_next() does the right thing.
        // remember: tail is currently executing.
        ll_push_back(&ready2run, &mainflow.llentry);
    }

    yield();
}
