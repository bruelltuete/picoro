#include "coroutine.h"
#include "pico/stdlib.h"
#include <string.h>


//struct Coroutine*   waiting4dma_head;
//struct Coroutine*   waiting4timer_head;


// head is currently running.
struct LinkedList   ready2run;


#define FLAGS_DO_NOT_RESCHEDULE     (1 << 0)        //< Once the coro ends up in the scheduler it will not be rescheduled, effectively exiting it.


struct CoroutineHeader
{
    volatile uint32_t*      sp;
    struct LinkedListEntry  llentry;
    uint32_t                flags;
}  mainflow;

static_assert(sizeof(mainflow.llentry) == 4);
// arm docs say that stack pointer should be 8 byte aligned, at a public interface.
// FIXME ...which is not what this assert here checks. 
static_assert((offsetof(struct Coroutine, stack) % 8) == 0);


void yield_and_exit()
{
    struct Coroutine* self = LL_ACCESS(self, llentry, ll_peek_head(&ready2run));
    self->flags |= FLAGS_DO_NOT_RESCHEDULE;
    yield();
}

// returns stack pointer for next coro
static volatile uint32_t* schedule_next(volatile uint32_t* current_sp)
{
    // FIXME: check waiting4timer etc as well.
    assert(!ll_is_empty(&ready2run));

    struct Coroutine* currentcoro = LL_ACCESS(currentcoro, llentry, ll_pop_front(&ready2run));
    currentcoro->sp = current_sp;

    if (!(currentcoro->flags & FLAGS_DO_NOT_RESCHEDULE))
        ll_push_back(&ready2run, &currentcoro->llentry);

    // FIXME: check waiting4timer etc as well.
    if (ll_is_empty(&ready2run))
        return mainflow.sp;

    struct Coroutine* upnext = LL_ACCESS(upnext, llentry, ll_peek_head(&ready2run));
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

    storage->flags = 0;
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

    if (!mainflow.sp)
    {
        ll_init_list(&ready2run);
        // i need mainflow in ready2run.head so that schedule_next() does the right thing.
        // remember: head is currently executing.
        ll_push_back(&ready2run, &mainflow.llentry);
        // mainflow.sp will be updated by yield().

        // run this pseudo coro only once (right now).
        mainflow.flags |= FLAGS_DO_NOT_RESCHEDULE;
    }

    ll_push_back(&ready2run, &storage->llentry);

    yield();
}
