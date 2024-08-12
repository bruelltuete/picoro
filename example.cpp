#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/platform.h"
#include "hardware/dma.h"
#include "hardware/sync.h"
#include "coroutine.h"


struct Coroutine<>    block1;
struct Coroutine<>    block2;
struct Coroutine<>    block3;

int dmawritechannel = -1;

uint32_t    readbuf = 0x1234;
uint32_t    writebuf;

static void __no_inline_not_in_flash_func(dma_irq_handler)()
{
    uint32_t save = save_and_disable_interrupts();

    assert((__get_current_exception() - 16) == DMA_IRQ_0);

    bool w = dma_irqn_get_channel_status(0, dmawritechannel);
    if (w)
    {
        dma_irqn_acknowledge_channel(0, dmawritechannel);
        // wake up coroutine_3 that was sleeping in yield_and_wait4wakeup().
        wakeup(&block3);
    }
    restore_interrupts(save);
}

static uint32_t coroutine_3(uint32_t param)
{
    dmawritechannel = dma_claim_unused_channel(true);
    dma_channel_config dmawritecfg = dma_channel_get_default_config(dmawritechannel);
    // we'll just read and write the same thing over and over again.
    channel_config_set_read_increment(&dmawritecfg, false);
    channel_config_set_write_increment(&dmawritecfg, false);
    channel_config_set_transfer_data_size(&dmawritecfg, DMA_SIZE_32);

    int dmatimer = dma_claim_unused_timer(true);
    dma_timer_set_fraction(dmatimer, 1, 10000);       // 120 MHz / 100
    channel_config_set_dreq(&dmawritecfg, dma_get_timer_dreq(dmatimer));
    dma_channel_set_config(dmawritechannel, &dmawritecfg, false);

    dma_channel_set_read_addr(dmawritechannel, &readbuf, false);
    dma_channel_set_write_addr(dmawritechannel, &writebuf, false);
    dma_channel_set_trans_count(dmawritechannel, 1000, false);

    // preemptive ack to clear whatever might be left pending from some other software component.
    dma_irqn_acknowledge_channel(0, dmawritechannel);
    // ...might otherwise get spurious dma-completions.
    dma_irqn_set_channel_enabled(0, dmawritechannel, true);

    irq_add_shared_handler(DMA_IRQ_0, dma_irq_handler, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    irq_set_enabled(DMA_IRQ_0, true);


    while (true)
    {
        dma_channel_start(dmawritechannel);
        yield_and_wait4wakeup();
        printf(".\n");
    }

    return 0;
}

static uint32_t coroutine_2(uint32_t param)
{
    yield_and_start(coroutine_3, 0, &block3);

    while (param > 0)
    {
        printf("B: %ld\n", param);
        --param;
        yield_and_wait4time(make_timeout_time_ms(900));
    }

    return 0;
}

static uint32_t coroutine_1(uint32_t param)
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
