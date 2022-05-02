#include "i2c.h"
#include "hardware/i2c.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include <string.h>
#include "coroutine.h"
#define RINGBUFFER_SIZE 4
#include "ringbuffer.h"


static int              dmaread[2] = {-1, -1};
static int              dmawrite[2] = {-1, -1};
static const int        dmairq[2] = {0, 0};
static volatile bool    datareadcaused[2];
static volatile bool    datawritecaused[2];


// FIXME: do we want one coro per i2c controller? or one for both controllers?
static Coroutine<256>      i2cdriverblock[2];

static struct CmdRingbufferEntry
{
    int                 numcmds;
    uint16_t*           cmds;
    int                 numresults;
    uint8_t*            results;
    Waitable            waitable;
    int8_t              address;
}                   cmdringbuffer[2][RINGBUFFER_SIZE];
static RingBuffer   cmdindices[2];
static Waitable     newcmdswaitable[2];


template <int I>
static void dma_irq_handler()
{
    bool r = dma_irqn_get_channel_status(dmairq[I], dmaread[I]);
    bool w = dma_irqn_get_channel_status(dmairq[I], dmawrite[I]);
    if (r)
        dma_irqn_acknowledge_channel(dmairq[I], dmaread[I]);
    if (w)
        dma_irqn_acknowledge_channel(dmairq[I], dmawrite[I]);

    datareadcaused[I] |= r;
    datawritecaused[I] |= w;

    if (datawritecaused[I] && datareadcaused[I])
        wakeup(&i2cdriverblock[I]);
}

static uint32_t i2cdriver_func(uint32_t param)
{
    i2c_inst_t* i2c = (i2c_inst_t*) param;
    int         i2cindex = i2c_hw_index(i2c);

    while (true)
    {
        if (!rb_is_empty(&cmdindices[i2cindex]))
        {
            // FIXME: previous dma might still be in progress!
            //        actually, it shouldnt... but might be worth asserting on.

            // not empty, so do the thing.
            CmdRingbufferEntry&  c = cmdringbuffer[i2cindex][rb_peek_front(&cmdindices[i2cindex])];

            // we will always have to do some writes.
            datawritecaused[i2cindex] = false;
            // but reads are optional. so if there are no reads then we pretend the read dma has finished already.
            datareadcaused[i2cindex] = c.numresults == 0;

            i2c->hw->enable = 0;
            i2c->hw->tar = c.address;
            i2c->hw->enable = 1;

            if (c.numresults > 0)
            {
                dma_channel_set_read_addr(dmaread[i2cindex], &i2c->hw->data_cmd, false);
                dma_channel_set_write_addr(dmaread[i2cindex], &c.results[0], false);
                dma_channel_set_trans_count(dmaread[i2cindex], c.numresults, false);
                dma_channel_start(dmaread[i2cindex]);
            }
            dma_channel_set_read_addr(dmawrite[i2cindex], &c.cmds[0], false);
            dma_channel_set_write_addr(dmawrite[i2cindex], &i2c->hw->data_cmd, false);
            dma_channel_set_trans_count(dmawrite[i2cindex], c.numcmds, false);
            dma_channel_start(dmawrite[i2cindex]);

            yield_and_wait4wakeup();

#ifndef NDEBUG
            c.numcmds = 0;
            c.cmds = (uint16_t*) 0xdeadbeef;
            c.numresults = 0;
            c.results = (uint8_t*) 0xdeadbeef;
#endif
            signal(&c.waitable);

            rb_pop_front(&cmdindices[i2cindex]);
        }

        yield_and_wait4signal(&newcmdswaitable[i2cindex]);

#ifndef NDEBUG
        {
            // if we've been signaled that there's a new cmd than there better be a new command!
            CmdRingbufferEntry&  c = cmdringbuffer[i2cindex][rb_peek_front(&cmdindices[i2cindex])];
            assert(c.numcmds > 0);
        }
#endif
    }
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
    if (dmaread[i2cindex] != -1)
        return;

    static const irq_handler_t dmahandlers[] = {dma_irq_handler<0>, dma_irq_handler<1>};

    rb_init_ringbuffer(&cmdindices[i2cindex]);
    memset(&newcmdswaitable[i2cindex], 0, sizeof(newcmdswaitable[i2cindex]));

    // side note: caller should have called i2c_init(), which enables transmit/receive dreq on the i2c side.
    i2c_inst_t* i2c = i2cinst_from_index(i2cindex);

    // dma channel for writing data to device
    {
        dmawrite[i2cindex] = dma_claim_unused_channel(true);
        dma_channel_config dmawritecfg = dma_channel_get_default_config(dmawrite[i2cindex]);
        // we are reading from a cpu array and writing it to the same i2c address
        channel_config_set_read_increment(&dmawritecfg, true);
        channel_config_set_write_increment(&dmawritecfg, false);

        // i2c transmits bytes as data, but the controller receives control bits in the higher bits of the word
        channel_config_set_transfer_data_size(&dmawritecfg, DMA_SIZE_16);
        channel_config_set_dreq(&dmawritecfg, i2c_get_dreq(i2c, true));
        dma_channel_set_config(dmawrite[i2cindex], &dmawritecfg, false);

        dma_irqn_set_channel_enabled(dmairq[i2cindex], dmawrite[i2cindex], true);
    }

    // dma channel for reading bytes coming from the chip.
    {
        dmaread[i2cindex] = dma_claim_unused_channel(true);
        dma_channel_config dmareadcfg = dma_channel_get_default_config(dmaread[i2cindex]);
        channel_config_set_read_increment(&dmareadcfg, false);  // always reading from i2c fifo register
        channel_config_set_write_increment(&dmareadcfg, true);  // writing sequentially into system mem

        channel_config_set_transfer_data_size(&dmareadcfg, DMA_SIZE_8);
        channel_config_set_dreq(&dmareadcfg, i2c_get_dreq(i2c, false));
        dma_channel_set_config(dmaread[i2cindex], &dmareadcfg, false);
        
        dma_irqn_set_channel_enabled(dmairq[i2cindex], dmaread[i2cindex], true);
    }

    irq_add_shared_handler(DMA_IRQ_0 + dmairq[i2cindex], dmahandlers[i2cindex], PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    irq_set_enabled(DMA_IRQ_0 + dmairq[i2cindex], true);

    yield_and_start(i2cdriver_func, (uint32_t) i2c, &i2cdriverblock[i2cindex]);
}

Waitable* queue_cmds(i2c_inst_t* i2c, int8_t address, int numcmds, uint16_t* cmds, int numresults, uint8_t* results)
{
    assert(numcmds > 0);
    // each read needs a read-command!
    assert(numcmds >= numresults);

    const int i2cindex = i2c_hw_index(i2c);

    init(i2cindex);

    while (rb_is_full(&cmdindices[i2cindex]))
    {
        // if cmd buffer is full then wait for current transfer to finish.
        // note that this is a loop because there might be another coro waiting for this transfer already and it might be woken sooner than us and queue another transfer in front of us.
        yield_and_wait4signal(&cmdringbuffer[i2cindex][rb_peek_front(&cmdindices[i2cindex])].waitable);
    }

    CmdRingbufferEntry&  c = cmdringbuffer[i2cindex][rb_push_back(&cmdindices[i2cindex])];
    c.numcmds = numcmds;
    c.cmds = cmds;
    c.numresults = numresults;
    c.results = results;
    c.address = address;

    // tell driver that there are new cmds waiting.
    signal(&newcmdswaitable[i2cindex]);

    return &c.waitable;
}
