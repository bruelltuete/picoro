#include <stdio.h>
#include "pico/stdlib.h"
#include "coroutine.h"


struct Coroutine    block1;
struct Coroutine    block2;

void coroutine_2(int param)
{
    while (param > 0)
    {
        printf("B: %d\n", param);
        --param;
        yield();
    }
}

void coroutine_1(int param)
{
    yield_and_start(coroutine_2, 5, &block2);

    while (param > 0)
    {
        printf("A: %d\n", param);
        --param;
        yield();
    }
}

int main() 
{
    stdio_init_all();

    ll_unit_test();

    printf("Hello, coroutine test!\n");

    yield_and_start(coroutine_1, 10, &block1);

    printf("done\n");
    return 0;
}
