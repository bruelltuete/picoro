#include "sk6812.h"
#include "sk6812.pio.h"
#include <cstring>
#include "picoro/coroutine.h"
#include "hardware/dma.h"


bool init_sk6812(SK6812Framebuffer* fb, PIO pio, int datapin)
{
    std::memset(fb, 0, sizeof(*fb));

    fb->pio = pio;
    bool ok2add = pio_can_add_program(pio, &sk6812_program);
    if (!ok2add)
        return false;

    fb->dmachannel = dma_claim_unused_channel(false);
    if (fb->dmachannel == -1)
        return false;

    fb->sm = pio_claim_unused_sm(pio, false);
    if (fb->sm == -1)
    {
        dma_channel_unclaim(fb->dmachannel);
        return false;
    }

    uint offset = pio_add_program(pio, &sk6812_program);

    static const int    freq_hz = 800000;
    sk6812_program_init(fb->pio, fb->sm, offset, datapin, freq_hz);

    int dreq = pio_get_dreq(fb->pio, fb->sm, true);
    dma_channel_config dmacfg = dma_channel_get_default_config(fb->dmachannel);
    channel_config_set_dreq(&dmacfg, dreq);
    channel_config_set_transfer_data_size(&dmacfg, DMA_SIZE_32);    // grbx
    channel_config_set_read_increment(&dmacfg, true);
    channel_config_set_write_increment(&dmacfg, false);
    dma_channel_configure(fb->dmachannel, &dmacfg, &pio->txf[fb->sm], &fb->grbx_pixels[0], count_of(fb->grbx_pixels), false);

    // FIXME: irq handler
    // FIXME: side note: pio doesnt have a "tx empty" interrupt, or "stalled" interrupt.
    //        so we'd have to wait for dma completion and then waste some time until the fifo has been emptied at 800khz

    //pio_remove_program
    return true;
}

static Coroutine<>     sk6812block;

static uint32_t sk6812func(uint32_t param)
{
    SK6812Framebuffer* fb = (SK6812Framebuffer*) param;

    //pio_sm_put_blocking(pio0, 0, pixel_grb << 8u);

    dma_channel_set_read_addr(fb->dmachannel, &fb->grbx_pixels[0], false);
    dma_channel_set_trans_count(fb->dmachannel, count_of(fb->grbx_pixels), true);

    // should be about 5 ms
    int transfertime_us = count_of(fb->grbx_pixels) * 24 * 1000000 / 800000;
    // FIXME: but round up for now until we have completion irq
    yield_and_wait4time(make_timeout_time_us(transfertime_us + 1000));
    return 0;
}

Waitable* update_sk6812ledstrip(SK6812Framebuffer* fb)
{
    yield_and_start(sk6812func, (uint32_t) fb, &sk6812block);
    // coro will exit when finished, it'll become signalled.
    return &sk6812block.waitable;
}
