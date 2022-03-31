#pragma once
#include "linkedlist.h"
#include "pico/stdlib.h"

struct Coroutine
{
    volatile uint32_t*      sp;
    struct LinkedListEntry  llentry;

    // FIXME: stack should be 8 byte aligned, i think.

    // stack should be small, no need for recursive/deep callstacks.
    // BUT: if you want to use printf you need lots more than 128*4=512 bytes of stack!
    volatile uint32_t   stack[256];
};

/**
 * Entry-point for our coroutine.
 * Looks like \code void myfunc(int param) \endcode
 */
typedef void (*coroutinefp_t)(int);


void yield_and_start(coroutinefp_t func, int param, struct Coroutine* storage);
void yield_and_exit();
void yield();
