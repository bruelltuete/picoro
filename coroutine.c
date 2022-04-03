#include "coroutine.h"
#include "pico/stdlib.h"
#include "pico/critical_section.h"
#include <string.h>


// head is currently running.
struct LinkedList   ready2run;
struct LinkedList   waiting4timer;
struct LinkedList   waiting4wakeup;
critical_section_t  lock;


#define FLAGS_DO_NOT_RESCHEDULE     (1 << 0)        //< Once the coro ends up in the scheduler it will not be rescheduled, effectively exiting it.
#define FLAGS_PUT_IN_TIMER_QUEUE    (1 << 1)
//#define FLAGS_PUT_IN_SLEEPING_QUEUE (1 << 2)


struct CoroutineHeader
{
    volatile uint32_t*      sp;
    struct LinkedListEntry  llentry;
    uint16_t                flags;
    int8_t                  sleepcount;
}  mainflow;

static_assert(sizeof(uint32_t*) == sizeof(uint32_t));
static_assert(sizeof(mainflow.llentry) == 4);
// arm docs say that stack pointer should be 8 byte aligned, at a public interface.
// FIXME ...which is not what this assert here checks. 
static_assert((offsetof(struct Coroutine, stack) % 8) == 0);


#if 0
static volatile bool* irqhandlercalled_12;
static void irqhandler_12()     // DMA_IRQ_1
{
    irq_remove_handler(DMA_IRQ_1, irqhandler_12);
    *irqhandlercalled_12 = true;
    // FIXME: might have to get out of a timed wait function, if thats a thing another coro is executing.
}

void yield_and_wait4irq(uint irqnum, volatile bool* handlercalledalready)
{
    // FIXME: how to add waiting4irq? schedule_next() needs the current coro in head. and each coro can only be in one queue.

    irq_handler_t   handler = NULL;

    switch (irqnum)
    {
        case DMA_IRQ_1: // 12
            irqhandlercalled_12 = handlercalledalready;
            handler = irqhandler_12;
            break;
        default:
            assert(false);
    }

    // FIXME: this is all quite ugly... i need handlercalledalready to handle a race with the caller's handler: irq might have already happened when cpu gets here
    //        but also if caller hooked up and enabled an irq without a handler, the irq might trigger before we get here.
    //        there's quite a lot of setup that needs to happen in the right order for this to work...
    //        and having handlercalledalready makes this whole function pointless i think.
    
    if (!(*handlercalledalready))
        irq_add_shared_handler(irqnum, handler, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);

    do
    {
        yield();
    }
    while (!(*handlercalledalready));
}
#endif

// returns stack pointer for next coro
static volatile uint32_t* schedule_next(volatile uint32_t* current_sp)
{
    critical_section_enter_blocking(&lock);

    assert(!ll_is_empty(&ready2run));
    struct Coroutine* currentcoro = LL_ACCESS(currentcoro, llentry, ll_pop_front(&ready2run));
    currentcoro->sp = current_sp;

    bool is_resched = !(currentcoro->flags & FLAGS_DO_NOT_RESCHEDULE);
    bool is_waiting4time = currentcoro->flags & FLAGS_PUT_IN_TIMER_QUEUE;
    bool is_sleeping = currentcoro->sleepcount > 0;

    if (is_waiting4time)
    {
        assert(!is_sleeping);
        ll_push_back(&waiting4timer, &currentcoro->llentry);
        is_resched = false;
    }
    if (is_sleeping)
    {
        assert(!is_waiting4time);
        ll_push_back(&waiting4wakeup, &currentcoro->llentry);
        is_resched = false;
    }

    if (is_resched)
        ll_push_back(&ready2run, &currentcoro->llentry);


    if (ll_is_empty(&ready2run))
    {
        while (!ll_is_empty(&waiting4wakeup))
        {
            critical_section_exit(&lock);
            // FIXME: wfe vs wfi? pico-sdk uses mostly wfe and sev for timer/alarm stuff.
            //        fwiw, using wfi here will block indefinitely most of the time. i wonder why though, the timer alarm is an irq.
            __wfe();
            critical_section_enter_blocking(&lock);

            if (!ll_is_empty(&ready2run))
                goto gotonext;
        }

        // nothing ready to run, check if there's anything waiting on timeout.
        // if timeout has not yet elapsed then sleep.
        // if nothing in timer queue then return to mainflow (nothing left to execute).

        critical_section_exit(&lock);
        return mainflow.sp;
    }

    struct Coroutine* upnext;
gotonext:
    upnext = LL_ACCESS(upnext, llentry, ll_peek_head(&ready2run));

    critical_section_exit(&lock);
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
    storage->sleepcount = 0;
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

    bool is_first_coro = false;
    if (!mainflow.sp)
    {
        is_first_coro = true;
        ll_init_list(&ready2run);
        ll_init_list(&waiting4timer);
        ll_init_list(&waiting4wakeup);
        critical_section_init(&lock);

        // i need mainflow in ready2run.head so that schedule_next() does the right thing.
        // remember: head is currently executing.
        ll_push_back(&ready2run, &mainflow.llentry);
        // mainflow.sp will be updated by yield().

        // run this pseudo coro only once (right now).
        mainflow.flags |= FLAGS_DO_NOT_RESCHEDULE;
    }

    critical_section_enter_blocking(&lock);
    ll_push_back(&ready2run, &storage->llentry);
    critical_section_exit(&lock);

    yield();

    if (is_first_coro)
    {
        // coroutines don't end up here.
        // but the mainflow does... and we'll need to cleanup.
        critical_section_deinit(&lock);
    }
}

void yield_and_exit()
{
    critical_section_enter_blocking(&lock);
    struct Coroutine* self = LL_ACCESS(self, llentry, ll_peek_head(&ready2run));
    self->flags |= FLAGS_DO_NOT_RESCHEDULE;
    critical_section_exit(&lock);

    yield();
}

void yield_and_wait4time(absolute_time_t until)
{
    critical_section_enter_blocking(&lock);

    // FIXME
    struct Coroutine* self = LL_ACCESS(self, llentry, ll_peek_head(&ready2run));
    //self->flags |= FLAGS_PUT_IN_TIMER_QUEUE;
    critical_section_exit(&lock);

    yield();
}

void yield_and_wait4wakeup()
{
    critical_section_enter_blocking(&lock);
    struct Coroutine* self = LL_ACCESS(self, llentry, ll_peek_head(&ready2run));
    self->sleepcount++;
    critical_section_exit(&lock);

    yield();
}

void wakeup(struct Coroutine* coro)
{
    // likely to be called from interrupt/exception handler!

    critical_section_enter_blocking(&lock);


    // beware: wakeup() might have been called too soon, before schedule_next() has had a chance to put it on waiting4wakeup.
    coro->sleepcount--;
    ll_remove(&waiting4wakeup, &coro->llentry);

    if (ll_peek_head(&ready2run) != &coro->llentry)
    {
        // FIXME: this could make coro the head of the queue! which to schedule_next() means it's running.
        ll_push_back(&ready2run, &coro->llentry);
    }

    critical_section_exit(&lock);

    // never ever call yield() here!
}
