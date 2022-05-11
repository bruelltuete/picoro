#pragma once
#include "linkedlist.h"
#include "pico/stdlib.h"

// if defined then we keep track of how much cpu time each coro gets.
// this is basically debugging or profiling. no other use really. so you can switch it off.
#ifndef PICORO_TRACK_EXECUTION_TIME
#define PICORO_TRACK_EXECUTION_TIME     0
#endif


// forward decl
struct CoroutineHeader;

struct Waitable
{
    CoroutineHeader*    waitchain;
    // FIXME: i'm not sure i want a counting semaphore...
    int8_t              semaphore;      // >0 means signalled.
};

struct CoroutineHeader
{
    Waitable                waitable;
    volatile uint32_t*      sp;
    struct LinkedListEntry  llentry;
    absolute_time_t         wakeuptime;
#if PICORO_TRACK_EXECUTION_TIME
    uint64_t                timespentexecuting; // in microseconds.
#endif
    uint32_t                exitcode;   // there's prob a way that we could recycle the stack to store this, given that the stack is no longer useful (coro exited, remember)
    uint16_t                stacksize;  // ideally we wouldnt need this one.
    uint8_t                 flags;
    int8_t                  sleepcount;

    // if PICO_USE_STACK_GUARDS is defined then 32 bytes of the stack are used as a guard area.
    // as opposed to protecting these header fields here.
};

template <int StackSize_ = 256>
struct Coroutine : CoroutineHeader
{
    static const int StackSize = StackSize_;

    // stack should be small, no need for recursive/deep callstacks.
    // BUT: if you want to use printf you need lots more than 128*4=512 bytes of stack!
    // the absolute minium stack size is 64*4=256 bytes. which is just enough to call yield_and_start() to start off a bunch of other coros.
    // BEWARE: the time/timer/sleep functions in the pico-sdk need a lot of stack! 150*4=600 bytes or more!
    volatile uint32_t       stack[StackSize]  __attribute__((aligned(32)));

#if PICO_USE_STACK_GUARDS
    // if we have stack guards then we loose 32 bytes of stack space (the guard area).
    static_assert(StackSize >= 64 + (32 / 4));
#else
    static_assert(StackSize >= 64);
#endif
};

/**
 * Entry-point for our coroutine.
 * Looks like \code uint32_t myfunc(int param) \endcode
 * The return value is the exit code, which can be queried later via FIXME.
 */
typedef uint32_t (*coroutinefp_t)(uint32_t);
static_assert(sizeof(uint32_t) >= sizeof(void*));

// stacksize unit is number of uint32_ts
extern void yield_and_start_ex(coroutinefp_t func, int param, CoroutineHeader* storage, int stacksize);

/**
 * @brief Exits the currently running coroutine by taking it off the scheduler and yielding.
 * Does not return.
 * The current coroutine will "signal" once it's dead, i.e. you can use yield_and_wait4signal() to wait for a coroutine to finish.
 * @param exitcode 
 */
extern void yield_and_exit(uint32_t exitcode = 0);
extern void yield_and_wait4time(absolute_time_t until);
extern void yield_and_wait4wakeup();
extern void yield();

/**
 * @brief Yields execution and starts another coroutine.
 * If called from an existing coroutine then it will eventually return.
 * If called from outside of coroutine it will only return when all currently running coroutines have finished.
 * A coroutine that has been previously started but has not exited yet is considered "live".
 * Starting a "live" coroutine does nothing, the attempt is ignored.
 * 
 * @warning Do not call from an IRQ handler! None of the yield() functions are safe to call from interrupts.
 * 
 * @param func entry point
 * @param param a value to pass to coroutine entry point
 * @param storage stack etc for this new coroutine
 */
template <int StackSize>
void yield_and_start(coroutinefp_t func, int param, struct Coroutine<StackSize>* storage)
{
    yield_and_start_ex(func, param, storage, StackSize);
}

/**
 * @brief Yields execution until "other" has exited/signaled.
 */
extern void yield_and_wait4signal(Waitable* other);
// FIXME: i prob want to wait for more than one... no idea how yet.
//extern "C" uint32_t yield_and_wait4others();

/**
 * @brief 
 * Safe to call from IRQ handler.
 */
extern void wakeup(CoroutineHeader* coro);

// Beware: do not mix wakeup() and signal()! I.e. yield_and_wait4wakeup() and wakeup() is fine; yield_and_wait4signal() and signal() is fine; but don't mix!
extern void signal(Waitable* waitable);

// FIXME: considered but prob a bad idea. too much caller specific application logic needs to happen in the right order to not loose an irq.
//extern void yield_and_wait4irq(uint irqnum, volatile bool* handlercalledalready);
