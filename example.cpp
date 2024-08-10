#include <stdio.h>
#include "pico/stdlib.h"
#include "coroutine.h"


struct Coroutine<>    block1;
struct Coroutine<>    block2;

uint32_t coroutine_2(uint32_t param)
{
    while (param > 0)
    {
        printf("B: %ld\n", param);
        --param;
        yield_and_wait4time(make_timeout_time_ms(900));
    }

    return 0;
}

uint32_t coroutine_1(uint32_t param)
{
    yield_and_start(coroutine_2, 50, &block2);

    while (param > 0)
    {
        printf("A: %ld\n", param);
        --param;
        yield_and_wait4time(make_timeout_time_ms(500));
    }

    return 0;
}

int main()
{
    stdio_init_all();

    ll_unit_test();

    printf("Hello, coroutine test!\n");

    yield_and_start(coroutine_1, 100, &block1);
    // will never get here: the scheduler never exits.
    printf("done?\n");
    return 0;
}
