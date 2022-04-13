#pragma once
#include "linkedlist.h"
#include "pico/stdlib.h"

struct CoroutineHeader
{
    volatile uint32_t*      sp;
    struct LinkedListEntry  llentry;
    absolute_time_t         wakeuptime;
    uint8_t                 flags;
    int8_t                  sleepcount;

    // FIXME: consider PMU-protecting some of these fields against stack overflow (sp underflowing stack into the header).
};

template <int StackSize_ = 256>
struct Coroutine : CoroutineHeader
{
    static const int StackSize = StackSize_;

    // stack should be small, no need for recursive/deep callstacks.
    // BUT: if you want to use printf you need lots more than 128*4=512 bytes of stack!
    // the absolute minium stack size is 64*4=256 bytes. which is just enough to call yield_and_start() to start off a bunch of other coros.
    // BEWARE: the time/timer/sleep functions in the pico-sdk need a lot of stack! 150*4=600 bytes or more!
    volatile uint32_t       stack[StackSize]  __attribute__((aligned(8)));
    static_assert(StackSize >= 64);
};

/**
 * Entry-point for our coroutine.
 * Looks like \code void myfunc(int param) \endcode
 */
typedef void (*coroutinefp_t)(int);


// stacksize unit is number of uint32_ts
extern "C" void yield_and_start_ex(coroutinefp_t func, int param, CoroutineHeader* storage, int stacksize);

/**
 * @brief Yields execution and starts another coroutine.
 * If called from an existing coroutine then it will eventually return.
 * If called from outside of coroutine it will only return when all currently running coroutines have finished.
 * @warning Do not call from an IRQ handler! None of the yield() functions are safe to call from interrupts.
 * @param func entry point
 * @param param a value to pass to coroutine entry point
 * @param storage stack etc for this new coroutine
 */
template <int StackSize>
void yield_and_start(coroutinefp_t func, int param, struct Coroutine<StackSize>* storage)
{
    yield_and_start_ex(func, param, storage, StackSize);
}

extern "C" void yield_and_exit();
extern "C" void yield();
extern "C" void yield_and_wait4time(absolute_time_t until);
extern "C" void yield_and_wait4wakeup();

/**
 * @brief 
 * Safe to call from IRQ handler.
 */
extern "C" void wakeup(CoroutineHeader* coro);

// FIXME: considered but prob a bad idea. too much caller specific application logic needs to happen in the right order to not loose an irq.
//extern void yield_and_wait4irq(uint irqnum, volatile bool* handlercalledalready);
