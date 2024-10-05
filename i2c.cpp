#include "i2c.h"
#include "hardware/i2c.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include <string.h>
#include "coroutine.h"
#include "profiler.h"
#define RINGBUFFER_SIZE 4
#include "ringbuffer.h"


#if PICORO_I2CDRV_IN_RAM
#define DRVFUNC(f)    __no_inline_not_in_flash_func(f)
#else
#define DRVFUNC(f)    f
#endif


//static i2c_inst_t* i2cinst_from_index(int index);


static const unsigned int        dmairq[2] = {1, 1};

struct CmdRingbufferEntry
{
    int                 numcmds;
    const uint16_t*     cmds;
    int                 numresults;
    uint8_t*            results;
    Waitable            waitable;
    bool*               success;
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

    volatile bool*      wasaborted;
    volatile bool       datareadcaused;
    volatile bool       datawritecaused;
    volatile bool       haswokenup;

    bool                drivershouldexit;

    DriverState()
        : dmareadchannel(-1)
    {
    }
}           driverstate[2];


static __force_inline i2c_inst_t* i2cinst_from_index(int index)
{
    switch (index)
    {
    case 0: return i2c0;
    case 1: return i2c1;
    default:
        assert(false);
    }
    __breakpoint();
    return NULL;
}

template <int I>
static void __no_inline_not_in_flash_func(i2chandler)()
{
    uint32_t save = save_and_disable_interrupts();
    {
        PROFILE_THIS_FUNC;

        assert((__get_current_exception() - 16) == (I2C0_IRQ + I));

        i2c_inst_t* i2c = i2cinst_from_index(I);
        uint32_t intstatus = i2c_get_hw(i2c)->intr_stat;

        if (intstatus & I2C_IC_INTR_STAT_R_TX_ABRT_BITS)
        {

            *driverstate[I].wasaborted = true;

            // we've got a race with i2c having been aborted and cancelling
            // the dma transfer. post-abort we may still end up stuff more tx bits in!
            // so disable i2c dma here.
            i2c_get_hw(i2c)->dma_cr = 0;

            // not sure which one tbh... lets cancel both.
            // note: right now we dont care how much of the request was completed, e.g. 50% was sent, don't care. if we did we'd have to check transfer_count before cancelling!
            dma_channel_abort(driverstate[I].dmareadchannel);
            dma_channel_abort(driverstate[I].dmawritechannel);

            // need to read to clear the irq (it always reads as zero, nothing useful to do with the result).
            // but clear only once dma has been cancelled!
            // i2c chip keeps the fifos clear as long as the abort is in progress. if we clear it too early
            // we can enter the race mentioned above.
            i2c_get_hw(i2c)->clr_tx_abrt;

            if (!driverstate[I].haswokenup)
            {
                driverstate[I].haswokenup = true;
                wakeup(&driverstate[I].i2cdriverblock);
            }
        }
        else    // notice the else here! abort condition wins over tx-empty!
        if (intstatus & I2C_IC_INTR_MASK_M_TX_EMPTY_BITS)
        {
            i2c_get_hw(i2c)->intr_mask &= ~I2C_IC_INTR_MASK_M_TX_EMPTY_BITS;

            // i2c controller has successfully pushed out all the bits.
            // now we are really done with the transfer.
            if (!driverstate[I].haswokenup)
            {
                driverstate[I].haswokenup = true;
                wakeup(&driverstate[I].i2cdriverblock);
            }
        }
    }
    restore_interrupts(save);
}

template <int I>
static void __no_inline_not_in_flash_func(dma_irq_handler)()
{
    uint32_t save = save_and_disable_interrupts();
    {
        PROFILE_THIS_FUNC;

        assert((__get_current_exception() - 16) == (DMA_IRQ_0 + dmairq[I]));

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
            // we really should wait for the i2c controller to push out all the bits, before claiming it's finished.
            // otherwise dma will just fill the tx fifo and claim it's done too soon. (which gets us all sorts of weird problems)
            i2c_inst_t* i2c = i2cinst_from_index(I);
            i2c_get_hw(i2c)->intr_mask |= I2C_IC_INTR_MASK_M_TX_EMPTY_BITS;

            // reset ack flags "after" handling. so that we trigger this only once!
            driverstate[I].datareadcaused = false;
            driverstate[I].datawritecaused = false;
        }
    }
    restore_interrupts(save);
}

static const irq_handler_t dmahandlers[] = {dma_irq_handler<0>, dma_irq_handler<1>};

static void DRVFUNC(deinit)(int i2cindex)
{
    PROFILE_THIS_FUNC;

    // should only be called once all queued up cmds have been drained.
    assert(rb_is_empty(&driverstate[i2cindex].cmdindices));

    // we do have exclusive use of the i2c irq.
    int i2cirq = i2cindex + I2C0_IRQ;
    irq_set_enabled(i2cirq, false);
    irq_remove_handler(i2cirq, (i2cindex == 0) ? i2chandler<0> : i2chandler<1>);

    dma_irqn_set_channel_enabled(dmairq[i2cindex], driverstate[i2cindex].dmareadchannel, false);
    dma_irqn_set_channel_enabled(dmairq[i2cindex], driverstate[i2cindex].dmawritechannel, false);
    irq_remove_handler(DMA_IRQ_0 + dmairq[i2cindex], dmahandlers[i2cindex]);

    dma_channel_unclaim(driverstate[i2cindex].dmareadchannel);
    driverstate[i2cindex].dmareadchannel = -1;
    dma_channel_unclaim(driverstate[i2cindex].dmawritechannel);
    driverstate[i2cindex].dmawritechannel = -1;
}

static uint32_t DRVFUNC(i2cdriver_func)(uint32_t param)
{
    PROFILE_THIS_FUNC;

    i2c_inst_t* i2c = (i2c_inst_t*) param;
    int         i2cindex = i2c_hw_index(i2c);

    while (true)
    {
        if (!rb_is_empty(&driverstate[i2cindex].cmdindices))
        {
            // tx fifo should be empty! we make sure of that after each transfer.
            assert(i2c_get_hw(i2c)->raw_intr_stat & I2C_IC_RAW_INTR_STAT_TX_EMPTY_BITS);

            // not empty, so do the thing.
            CmdRingbufferEntry&  c = driverstate[i2cindex].cmdringbuffer[rb_peek_front(&driverstate[i2cindex].cmdindices)];

            // we will always have to do some writes.
            driverstate[i2cindex].datawritecaused = false;
            // but reads are optional. so if there are no reads then we pretend the read dma has finished already.
            driverstate[i2cindex].datareadcaused = c.numresults == 0;

            bool    wasaborted = false;
            driverstate[i2cindex].wasaborted = &wasaborted;
            driverstate[i2cindex].haswokenup = false;

            i2c_get_hw(i2c)->enable = 0;
            i2c_get_hw(i2c)->tar = c.address;
            i2c_get_hw(i2c)->intr_mask = I2C_IC_INTR_MASK_M_TX_ABRT_BITS;
            // explicitly switch on tx/rx dma signals on the i2c side.
            // we may have disabled those earlier. so re-enable.
            i2c_get_hw(i2c)->dma_cr = I2C_IC_DMA_CR_TDMAE_BITS | I2C_IC_DMA_CR_RDMAE_BITS;
            i2c_get_hw(i2c)->enable = 1;

            const int i2cirq = i2c_hw_index(i2c) + I2C0_IRQ;
            irq_set_enabled(i2cirq, true);

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
            // we should only ever get here until after all the bits in the tx fifo have been sent out!
            assert(i2c_get_hw(i2c)->raw_intr_stat & I2C_IC_RAW_INTR_STAT_TX_EMPTY_BITS);

            irq_set_enabled(i2cirq, false);

            if (c.success != NULL)
                *c.success = !wasaborted;

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

static void DRVFUNC(init)(int i2cindex)
{
    PROFILE_THIS_FUNC;

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

        // preemptive ack to clear whatever might be left pending from some other software component.
        dma_irqn_acknowledge_channel(dmairq[i2cindex], driverstate[i2cindex].dmawritechannel);
        // ...might otherwise get spurious dma-completions.
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

        // preemptive ack to clear whatever might be left pending from some other software component.
        dma_irqn_acknowledge_channel(dmairq[i2cindex], driverstate[i2cindex].dmareadchannel);
        // ...might otherwise get spurious dma-completions.
        dma_irqn_set_channel_enabled(dmairq[i2cindex], driverstate[i2cindex].dmareadchannel, true);
    }

    irq_add_shared_handler(DMA_IRQ_0 + dmairq[i2cindex], dmahandlers[i2cindex], PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    irq_set_enabled(DMA_IRQ_0 + dmairq[i2cindex], true);

    // we don't have exclusive use of the dma(irq), but we do have exclusive use of the i2c controller!
    int i2cirq = i2c_hw_index(i2c) + I2C0_IRQ;
    irq_set_exclusive_handler(i2cirq, (i2cindex == 0) ? i2chandler<0> : i2chandler<1>);

    yield_and_start(i2cdriver_func, (uint32_t) i2c, &driverstate[i2cindex].i2cdriverblock);
}

Waitable* DRVFUNC(queue_cmds)(i2c_inst_t* i2c, int8_t address, int numcmds, const uint16_t* cmds, int numresults, uint8_t* results, bool* success)
{
    PROFILE_THIS_FUNC;

    assert(numcmds > 0);
    // each read needs a read-command!
    assert(numcmds >= numresults);

    const int i2cindex = i2c_hw_index(i2c);

    init(i2cindex);

    while (rb_is_full(&driverstate[i2cindex].cmdindices))
    {
        // if cmd buffer is full then wait for current transfer to finish.
        // note that this is a loop because there might be another coro waiting for this transfer already and it might be woken sooner than us and queue another transfer in front of us.
        // FIXME: i dont think this works... what if a client of this driver is waiting on that waitable too?
        __breakpoint();
        yield_and_wait4signal(&driverstate[i2cindex].cmdringbuffer[rb_peek_front(&driverstate[i2cindex].cmdindices)].waitable);
    }

    CmdRingbufferEntry&  c = driverstate[i2cindex].cmdringbuffer[rb_push_back(&driverstate[i2cindex].cmdindices)];
    c.numcmds = numcmds;
    c.cmds = cmds;
    c.numresults = numresults;
    c.results = results;
    c.address = address;
    c.success = success;

    // tell driver that there are new cmds waiting.
    signal(&driverstate[i2cindex].newcmdswaitable);

    return &c.waitable;
}

const CoroutineHeader* DRVFUNC(get_driver_coro)(i2c_inst_t* i2c)
{
    PROFILE_THIS_FUNC;

    const int i2cindex = i2c_hw_index(i2c);

    if (driverstate[i2cindex].dmareadchannel == -1)
        return NULL;

    return &driverstate[i2cindex].i2cdriverblock;
}

void DRVFUNC(stop_i2c_driver_async)()
{
    PROFILE_THIS_FUNC;

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
