#include "i2c.h"
#include "hardware/i2c.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include <string.h>
#include "coroutine.h"
#define RINGBUFFER_SIZE 4
#include "ringbuffer.h"


static const int        dmairq[2] = {1, 1};

struct CmdRingbufferEntry
{
    int                 numcmds;
    uint16_t*           cmds;
    int                 numresults;
    uint8_t*            results;
    Waitable            waitable;
    int8_t              address;
};

static struct DriverState
{
    int                 dmareadchannel;
    int                 dmawritechannel;

    RingBuffer          cmdindices;
    Coroutine<256>      i2cdriverblock;
    Waitable            newcmdswaitable;

    CmdRingbufferEntry  cmdringbuffer[RINGBUFFER_SIZE];

    volatile bool       datareadcaused;
    volatile bool       datawritecaused;
    volatile bool       drivershouldexit;

    DriverState()
        : dmareadchannel(-1)
    {
    }
}           driverstate[2];


template <int I>
static void dma_irq_handler()
{
    uint ex = __get_current_exception();
    assert((ex - 16) == (DMA_IRQ_0 + dmairq[I]));

    // FIXME: workaround for bug in pico-sdk. cannot remove and re-add a shared handler
    //        so we leave ours here dangling and deal with that fact.
    if (driverstate[I].dmareadchannel == -1)
        return;

    bool r = dma_irqn_get_channel_status(dmairq[I], driverstate[I].dmareadchannel);
    bool w = dma_irqn_get_channel_status(dmairq[I], driverstate[I].dmawritechannel);
    if (r)
        dma_irqn_acknowledge_channel(dmairq[I], driverstate[I].dmareadchannel);
    if (w)
        dma_irqn_acknowledge_channel(dmairq[I], driverstate[I].dmawritechannel);

    driverstate[I].datareadcaused |= r;
    driverstate[I].datawritecaused |= w;

    if (driverstate[I].datawritecaused && driverstate[I].datareadcaused)
    {
        wakeup(&driverstate[I].i2cdriverblock);
        // reset ack flags "after" handling. so that we trigger this only once!
        driverstate[I].datareadcaused = false;
        driverstate[I].datawritecaused = false;
    }
}

static const irq_handler_t dmahandlers[] = {dma_irq_handler<0>, dma_irq_handler<1>};

static void deinit(int i2cindex)
{
    // should only be called once all queued up cmds have been drained.
    assert(rb_is_empty(&driverstate[i2cindex].cmdindices));

    dma_irqn_set_channel_enabled(dmairq[i2cindex], driverstate[i2cindex].dmareadchannel, false);
    dma_irqn_set_channel_enabled(dmairq[i2cindex], driverstate[i2cindex].dmawritechannel, false);
    
    // FIXME: pico-sdk has a bug in add/remove shared handler.
    //        so we'll never remove our handlers here and make sure to add them only once in init().
    //        and also the handler can deal with being called when it should have been registered.
    //irq_remove_handler(DMA_IRQ_0 + dmairq[i2cindex], dmahandlers[i2cindex]);

    dma_channel_unclaim(driverstate[i2cindex].dmareadchannel);
    driverstate[i2cindex].dmareadchannel = -1;
    dma_channel_unclaim(driverstate[i2cindex].dmawritechannel);
    driverstate[i2cindex].dmawritechannel = -1;
}

static uint32_t i2cdriver_func(uint32_t param)
{
    i2c_inst_t* i2c = (i2c_inst_t*) param;
    int         i2cindex = i2c_hw_index(i2c);

    while (true)
    {
        if (!rb_is_empty(&driverstate[i2cindex].cmdindices))
        {
            // FIXME: previous dma might still be in progress!
            //        actually, it shouldnt... but might be worth asserting on.

            // not empty, so do the thing.
            CmdRingbufferEntry&  c = driverstate[i2cindex].cmdringbuffer[rb_peek_front(&driverstate[i2cindex].cmdindices)];

            // we will always have to do some writes.
            driverstate[i2cindex].datawritecaused = false;
            // but reads are optional. so if there are no reads then we pretend the read dma has finished already.
            driverstate[i2cindex].datareadcaused = c.numresults == 0;

            i2c->hw->enable = 0;
            i2c->hw->tar = c.address;
            i2c->hw->enable = 1;

            if (c.numresults > 0)
            {
                dma_channel_set_read_addr(driverstate[i2cindex].dmareadchannel, &i2c->hw->data_cmd, false);
                dma_channel_set_write_addr(driverstate[i2cindex].dmareadchannel, &c.results[0], false);
                dma_channel_set_trans_count(driverstate[i2cindex].dmareadchannel, c.numresults, false);
                dma_channel_start(driverstate[i2cindex].dmareadchannel);
            }
            dma_channel_set_read_addr(driverstate[i2cindex].dmawritechannel, &c.cmds[0], false);
            dma_channel_set_write_addr(driverstate[i2cindex].dmawritechannel, &i2c->hw->data_cmd, false);
            dma_channel_set_trans_count(driverstate[i2cindex].dmawritechannel, c.numcmds, false);
            dma_channel_start(driverstate[i2cindex].dmawritechannel);

            yield_and_wait4wakeup();

#ifndef NDEBUG
            c.numcmds = 0;
            c.cmds = (uint16_t*) 0xdeadbeef;
            c.numresults = 0;
            c.results = (uint8_t*) 0xdeadbeef;
#endif
            signal(&c.waitable);

            rb_pop_front(&driverstate[i2cindex].cmdindices);
        }

        // FIXME: i think i need a timeout-able wait4signal, so that the driver can shutdown.
        yield_and_wait4signal(&driverstate[i2cindex].newcmdswaitable);
        if (driverstate[i2cindex].drivershouldexit && rb_is_empty(&driverstate[i2cindex].cmdindices))
            break;

#ifndef NDEBUG
        {
            // if we've been signaled that there's a new cmd than there better be a new command!
            CmdRingbufferEntry&  c = driverstate[i2cindex].cmdringbuffer[rb_peek_front(&driverstate[i2cindex].cmdindices)];
            assert(c.numcmds > 0);
        }
#endif
    }

    deinit(i2cindex);
    return 0;
}

static i2c_inst_t* i2cinst_from_index(int index)
{
    switch (index)
    {
    case 0: return i2c0;
    case 1: return i2c1;
    default:
        assert(false);
    }
    assert(false);
}

static void init(int i2cindex)
{
    if (driverstate[i2cindex].dmareadchannel != -1)
        return;

    driverstate[i2cindex].drivershouldexit = false;
    rb_init_ringbuffer(&driverstate[i2cindex].cmdindices);
    memset(&driverstate[i2cindex].newcmdswaitable, 0, sizeof(driverstate[i2cindex].newcmdswaitable));
    memset(&driverstate[i2cindex].i2cdriverblock, 0, sizeof(driverstate[i2cindex].i2cdriverblock));

    // side note: caller should have called i2c_init(), which enables transmit/receive dreq on the i2c side.
    i2c_inst_t* i2c = i2cinst_from_index(i2cindex);

    // dma channel for writing data to device
    {
        driverstate[i2cindex].dmawritechannel = dma_claim_unused_channel(true);
        dma_channel_config dmawritecfg = dma_channel_get_default_config(driverstate[i2cindex].dmawritechannel);
        // we are reading from a cpu array and writing it to the same i2c address
        channel_config_set_read_increment(&dmawritecfg, true);
        channel_config_set_write_increment(&dmawritecfg, false);

        // i2c transmits bytes as data, but the controller receives control bits in the higher bits of the word
        channel_config_set_transfer_data_size(&dmawritecfg, DMA_SIZE_16);
        channel_config_set_dreq(&dmawritecfg, i2c_get_dreq(i2c, true));
        dma_channel_set_config(driverstate[i2cindex].dmawritechannel, &dmawritecfg, false);

        dma_irqn_set_channel_enabled(dmairq[i2cindex], driverstate[i2cindex].dmawritechannel, true);
    }

    // dma channel for reading bytes coming from the chip.
    {
        driverstate[i2cindex].dmareadchannel = dma_claim_unused_channel(true);
        dma_channel_config dmareadcfg = dma_channel_get_default_config(driverstate[i2cindex].dmareadchannel);
        channel_config_set_read_increment(&dmareadcfg, false);  // always reading from i2c fifo register
        channel_config_set_write_increment(&dmareadcfg, true);  // writing sequentially into system mem

        channel_config_set_transfer_data_size(&dmareadcfg, DMA_SIZE_8);
        channel_config_set_dreq(&dmareadcfg, i2c_get_dreq(i2c, false));
        dma_channel_set_config(driverstate[i2cindex].dmareadchannel, &dmareadcfg, false);
        
        dma_irqn_set_channel_enabled(dmairq[i2cindex], driverstate[i2cindex].dmareadchannel, true);
    }

    // FIXME: workaround for bug in pico-sdk. remove and re-adding a shared handler does not work.
    //        so make sure we add our dma handler only once and never remove it again.
    static bool alreadyadded[2] = {false, false};
    if (!alreadyadded[i2cindex])
    {
        irq_add_shared_handler(DMA_IRQ_0 + dmairq[i2cindex], dmahandlers[i2cindex], PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
        alreadyadded[i2cindex] = true;
    }
    irq_set_enabled(DMA_IRQ_0 + dmairq[i2cindex], true);

    yield_and_start(i2cdriver_func, (uint32_t) i2c, &driverstate[i2cindex].i2cdriverblock);
}

Waitable* queue_cmds(i2c_inst_t* i2c, int8_t address, int numcmds, uint16_t* cmds, int numresults, uint8_t* results)
{
    assert(numcmds > 0);
    // each read needs a read-command!
    assert(numcmds >= numresults);

    const int i2cindex = i2c_hw_index(i2c);

    init(i2cindex);

    while (rb_is_full(&driverstate[i2cindex].cmdindices))
    {
        // if cmd buffer is full then wait for current transfer to finish.
        // note that this is a loop because there might be another coro waiting for this transfer already and it might be woken sooner than us and queue another transfer in front of us.
        yield_and_wait4signal(&driverstate[i2cindex].cmdringbuffer[rb_peek_front(&driverstate[i2cindex].cmdindices)].waitable);
    }

    CmdRingbufferEntry&  c = driverstate[i2cindex].cmdringbuffer[rb_push_back(&driverstate[i2cindex].cmdindices)];
    c.numcmds = numcmds;
    c.cmds = cmds;
    c.numresults = numresults;
    c.results = results;
    c.address = address;

    // tell driver that there are new cmds waiting.
    signal(&driverstate[i2cindex].newcmdswaitable);

    return &c.waitable;
}

const CoroutineHeader* get_driver_coro(i2c_inst_t* i2c)
{
    const int i2cindex = i2c_hw_index(i2c);

    if (driverstate[i2cindex].dmareadchannel == -1)
        return NULL;

    return &driverstate[i2cindex].i2cdriverblock;
}

void stop_i2c_driver_async()
{
    if (driverstate[0].dmareadchannel != -1)
    {
        driverstate[0].drivershouldexit = true;
        signal(&driverstate[0].newcmdswaitable);
    }

    if (driverstate[1].dmareadchannel != -1)
    {
        driverstate[1].drivershouldexit = true;
        signal(&driverstate[1].newcmdswaitable);
    }
}
