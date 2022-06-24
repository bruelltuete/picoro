#include "profiler.h"
#include <stdio.h>
#include "hardware/timer.h"
#include "hardware/irq.h"
#include "pico/platform.h"


#if PICORO_ENABLE_PROFILER

#define ALARM_NUM           0
#define SAMPLE_INTERVAL     1000

volatile FuncProfile*   currentfunction = 0;
static LinkedList       funclist;
static unsigned int     unaccountedsamples = 0;

// forward decl
void start_sampler();


static struct InitHelper
{
    InitHelper()
    {
        ll_init_list(&funclist);
        start_sampler();
    }
}       inithelper;


static void __no_inline_not_in_flash_func(alarm_irq)(void)
{
    // alarm will disarm itself after firing.
    // but we still need to clear the interrupt flag.
    hw_clear_bits(&timer_hw->intr, 1u << ALARM_NUM);

    if (currentfunction != 0)
        currentfunction->samples++;
    else
        unaccountedsamples++;

    // re-arm.
    // timer is running off the watchdog, which ticks at 1 microsecond intervals.
    // so lets say we are sampling every 1ms.
    timer_hw->alarm[ALARM_NUM] += SAMPLE_INTERVAL;
}

void linkup_func(FuncProfile* func)
{
    ll_push_back(&funclist, &func->llentry);
}

void start_sampler()
{
    hardware_alarm_claim(ALARM_NUM);

    irq_set_exclusive_handler(TIMER_IRQ_0 + ALARM_NUM, alarm_irq);
    irq_set_priority(TIMER_IRQ_0 + ALARM_NUM, 0);
    irq_set_enabled(TIMER_IRQ_0 + ALARM_NUM, true);

    hw_set_bits(&timer_hw->inte, 1u << ALARM_NUM);
    timer_hw->alarm[ALARM_NUM] = timer_hw->timelr + SAMPLE_INTERVAL;

    // FIXME: nmi?
}

void stop_and_dump_profile()
{
    // disarm.
    hw_set_bits(&timer_hw->armed, 1u << ALARM_NUM);
    // do not generate new interrups.
    hw_clear_bits(&timer_hw->inte, 1u << ALARM_NUM);
    // we dont want to handle anymore.
    irq_set_enabled(TIMER_IRQ_0 + ALARM_NUM, false);
    hardware_alarm_unclaim(ALARM_NUM);

    while (!ll_is_empty(&funclist))
    {
        FuncProfile* f = LL_ACCESS(f, llentry, ll_pop_front(&funclist));

        // ugh... slash is one of the few characters that will never(?) appear in a function name, so we can csv-split on it.
        printf("%s / %d / entries / %d / samples\n", f->name, f->entries, f->samples);
    }
    printf("unaccounted / / / %d / samples\n", unaccountedsamples);
}

#endif
