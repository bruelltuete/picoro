#pragma once
#include "linkedlist.h"
#include "pico/stdlib.h"

struct Coroutine
{
    volatile uint32_t*      sp;
    struct LinkedListEntry  llentry;
    uint16_t                flags;
    int8_t                  sleepcount;

    // stack should be small, no need for recursive/deep callstacks.
    // BUT: if you want to use printf you need lots more than 128*4=512 bytes of stack!
    volatile uint32_t       stack[256]  __attribute__((aligned(8)));
};

/**
 * Entry-point for our coroutine.
 * Looks like \code void myfunc(int param) \endcode
 */
typedef void (*coroutinefp_t)(int);


/**
 * @brief Yields execution and starts another coroutine.
 * If called from an existing coroutine then it will eventually return.
 * If called from outside of coroutine it will only return when all currently running coroutines have finished.
 * @param func entry point
 * @param param a value to pass to coroutine entry point
 * @param storage stack etc for this new coroutine
 */
extern void yield_and_start(coroutinefp_t func, int param, struct Coroutine* storage);
extern void yield_and_exit();
extern void yield();
extern void yield_and_wait4time(absolute_time_t until);
extern void yield_and_wait4wakeup();

extern void wakeup(struct Coroutine* coro);

// FIXME: considered but prob a bad idea. too much caller specific application logic needs to happen in the right order to not loose an irq.
//extern void yield_and_wait4irq(uint irqnum, volatile bool* handlercalledalready);
