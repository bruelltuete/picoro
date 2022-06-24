#include "coroutine.h"
#include "profiler.h"
#include "pico/stdlib.h"
#include "pico/critical_section.h"
#include "hardware/clocks.h"
#include "hardware/structs/mpu.h"
#include "hardware/regs/syscfg.h"
#include "hardware/structs/syscfg.h"
#include <string.h>


// head is currently running.
static struct LinkedList    ready2run;
static struct LinkedList    waiting4timer;
static critical_section_t   lock;
#if PICORO_TRACK_EXECUTION_TIME
static absolute_time_t      headrunningsince;   //< Head of ready2run running since this timestamp, in microseconds. Used to update timespentexecuting.
#endif

#define FLAGS_DO_NOT_RESCHEDULE     (1 << 1)        //< Once the coro ends up in the scheduler it will not be rescheduled, effectively exiting it.

// only needs the header, no need for stack.
static CoroutineHeader     mainflow;

static bool isdebuggerattached = false;     //< False if we think it's unlikely that a debugger is attached. True if we are pretty sure there is one.

// separate stack for schedule_next(), otherwise every coro would have to provision extra stack space for it.
// (instead of only once here)
// FIXME: consider putting this into scratch_y section! (which has 4k space, 2k for mainflow core0 stack, so 2k left for us)
static volatile uint32_t scheduler_stack[256]  __attribute__((aligned(32)));

static_assert(sizeof(uint32_t*) == sizeof(uint32_t));
static_assert(sizeof(mainflow.llentry) == 4);
// arm docs say that stack pointer should be 8 byte aligned, at a public interface.
// FIXME ...which is not what this assert here checks. 
static_assert((offsetof(struct Coroutine<>, stack) % 32) == 0);
//static_assert(((int32_t) &((Coroutine<>*) 0)->stack[0]) % 8 == 0);


static alarm_id_t       soonestalarmid = 0;
static absolute_time_t  soonesttime2wake = at_the_end_of_time;

// forward decls
static void prime_scheduler_timer_locked();
static void wakeup_locked(CoroutineHeader* coro);
static bool is_live(CoroutineHeader* storage, int stacksize);
static void uninstall_stack_guard(void* stacktop);


static int64_t timeouthandler(alarm_id_t id, CoroutineHeader* coro)
{
    PROFILE_THIS_FUNC;

    assert(coro != NULL);

    critical_section_enter_blocking(&lock);

    // this should be the case.
    // but might not be guaranteed???
    assert(&coro->llentry == ll_peek_head(&waiting4timer));

    wakeup_locked(coro);

    // we'll have to re-arm the timer with whatever the next up timeout is!
    soonesttime2wake = at_the_end_of_time;
    prime_scheduler_timer_locked();

    // FIXME: no need for sev here? wfe in schedule_next() wakes up without it. because wfe also wakes on interrupts???
    //__sev();

    critical_section_exit(&lock);
    return 0;
}

// assumes it gets called with lock held (or an equivalent of that).
static void prime_scheduler_timer_locked()
{
    PROFILE_THIS_FUNC;

    while (true)
    {
        CoroutineHeader* waiting4timeoutcoro = LL_ACCESS(waiting4timeoutcoro, llentry, ll_peek_head(&waiting4timer));
        if (waiting4timeoutcoro != NULL)
        {
            if (to_us_since_boot(waiting4timeoutcoro->wakeuptime) < to_us_since_boot(soonesttime2wake))
            {
                if (soonestalarmid != 0)
                    cancel_alarm(soonestalarmid);
                soonesttime2wake = waiting4timeoutcoro->wakeuptime;
                // FIXME: replace sdk alarm stuff with raw hw alarm.
                soonestalarmid = add_alarm_at(soonesttime2wake, (alarm_callback_t) timeouthandler, waiting4timeoutcoro, false);
                assert(soonestalarmid != -1);   // error
                if (soonestalarmid == 0)
                {
                    // timeout has expired already, back on the run queue.
                    ll_pop_front(&waiting4timer);
                    // it may be tempting to put waiting4timeoutcoro in the front of ready2run, given that it's already late for its turn.
                    // BUT: the head of ready2run may currently be executing! and we've been called from a timer irq.
                    // cannot just swap out the currently running task! that'd be preemptive multitasking. we are doing cooperative multitasking.
                    ll_push_back(&ready2run, &waiting4timeoutcoro->llentry);
                    // the equivalent of wakeup(). someone put the coro on the wait queue and inc'd sleepcount. if we take it off we need to dec!
                    waiting4timeoutcoro->sleepcount--;
                    // need to set up a timer for the coro waiting next up!
                    soonesttime2wake = at_the_end_of_time;
                    continue;
                }
            }
        }
        break;
    }
}

// returns stack pointer for next coro
// the extern-C is here because i want an unmangled name that's easy to be called from yield()'s asm section.
extern "C" volatile uint32_t* schedule_next(volatile uint32_t* current_sp)
{
    PROFILE_THIS_FUNC;

    critical_section_enter_blocking(&lock);

    // there should always be at least the currently running coro in ready2run.
    assert(!ll_is_empty(&ready2run));

    // scoping to avoid too much reach for currentcoro.
    {
        struct CoroutineHeader* currentcoro = LL_ACCESS(currentcoro, llentry, ll_pop_front(&ready2run));
        currentcoro->sp = current_sp;
#if PICORO_TRACK_EXECUTION_TIME
        currentcoro->timespentexecuting += absolute_time_diff_us(headrunningsince, get_absolute_time());
#endif

        // FIXME: i dont like how mainflow, which we only use 1x for init, creeps into every invocation of the scheduler.
        if (currentcoro != &mainflow)
        {
            // debug: hopefully we'll catch mem corruption cases earlier than the hardfault exception
            assert(is_live(currentcoro, currentcoro->stacksize));

            bool is_resched = !(currentcoro->flags & FLAGS_DO_NOT_RESCHEDULE);
            bool is_sleeping = currentcoro->sleepcount > 0;

            // here, this indicates that currentcoro is exiting.
            if (!is_resched)
            {
                // this assert can fail if we have a mismatched wait4time and wake.
                assert(!is_sleeping);

                // mark stack pointer as invalid.
                // trying to resume this will crash very quickly.
                // and, this makes sure that is_live() doesnt randomly stumble over old values we left in ram from a previous run.
                currentcoro->sp = (uint32_t*) 1;

                // when a coro exits the semaphore count doesnt matter: anyone who waits will be woken up.
                currentcoro->waitable.semaphore = 0x7F;
                if (currentcoro->waitable.waitchain)
                {
                    wakeup_locked(currentcoro->waitable.waitchain);  // FIXME: do proper chain stuff!
                    currentcoro->waitable.waitchain = NULL;
                }

#if PICO_USE_STACK_GUARDS
                uninstall_stack_guard((void*) &((Coroutine<>*) currentcoro)->stack[0]);
#endif
            }

            if (is_sleeping)
            {
                is_resched = false;

                if (is_at_the_end_of_time(currentcoro->wakeuptime))
                    ll_push_back(&waiting4timer, &currentcoro->llentry);
                else
                    ll_sorted_insert<offsetof(CoroutineHeader, wakeuptime) - offsetof(CoroutineHeader, llentry), uint64_t>(&waiting4timer, &currentcoro->llentry);
            }

            if (is_resched)
                ll_push_back(&ready2run, &currentcoro->llentry);
        }
    } // scoping for var visibility

    prime_scheduler_timer_locked();

    while (ll_is_empty(&ready2run))
    {
        // anyone waiting for timeout or for explicit wakeup?
        if (ll_is_empty(&waiting4timer))
        {
            // if nothing in timer queue then return to mainflow (nothing left to execute).
            critical_section_exit(&lock);
            return mainflow.sp;
        }

        critical_section_exit(&lock);

        check_debugger_attached();

        // FIXME: wfe vs wfi? pico-sdk uses mostly wfe and sev for timer/alarm stuff.
        //        fwiw, using wfi here will block indefinitely most of the time. i wonder why though, the timer alarm is an irq.
        // FIXME: need to test whether wakeup works... up to now, the interrupt handler was called too quickly

        // if we dont have a specific time to wake up at then just wait for anything...
        // might be spurious wakeup. in that case just re-check if we have anything to execute right now and if not sleep again.
        __wfe();

        // FIXME: do we need to wait for clocks to run again before continuing? docs dont say.
        assert((clocks_hw->enabled0 & clocks_hw->wake_en0) == clocks_hw->wake_en0);
        assert((clocks_hw->enabled1 & clocks_hw->wake_en1) == clocks_hw->wake_en1);

        critical_section_enter_blocking(&lock);

        // remember: this is all single threaded, so the only functions that could have modified the lists are interrupt handlers.
        // things like wakeup().
    }

    struct CoroutineHeader* upnext = LL_ACCESS(upnext, llentry, ll_peek_head(&ready2run));
    assert(upnext->sleepcount <= 0);
#if PICORO_TRACK_EXECUTION_TIME
    headrunningsince = get_absolute_time();
#endif
    critical_section_exit(&lock);
    check_debugger_attached();
    return upnext->sp;
}

void __attribute__ ((naked)) yield1(volatile uint32_t* schedsp)
{
    __asm volatile (
        // old stack is still active
        "push {lr};"
        "push {r0, r1, r2, r3, r4, r5, r6, r7};"
        // push only has encoding up to r7, so to push the other registers we need to copy them to r0-7 first.
        "mov r1, r8;"
        "mov r2, r9;"
        "mov r3, r10;"
        "mov r4, r11;"
        "mov r5, r12;"
        // r13 = stack pointer, r14 = link register, r15 = pc.
        "push {r1, r2, r3, r4, r5};"
        // insight: we do not need to preserve pc! execution will always continue from here on (just with a different stack).

        "mov r1, sp;"     // capture stack for current coro
        "mov sp, r0;"     // switch to scheduler stack

        "mov r0, r1;"
        "bl schedule_next;"
        "mov sp, r0;"     // activate stack for new coro

        // restore all those registers with the new stack.
        "pop {r1, r2, r3, r4, r5};"
        "mov r8, r1;"
        "mov r9, r2;"
        "mov r10, r3;"
        "mov r11, r4;"
        "mov r12, r5;"
        "pop {r0, r1, r2, r3, r4, r5, r6, r7};"
        "pop {pc};"     // return to caller, straight-forward box-standard nothing-fancy return.
        : // out
        : // in
        : "memory"  // clobber: make sure compiler has generated stores before and loads after this block.
    );

    // will not get here.
    __breakpoint();
}

void yield()
{
    PROFILE_THIS_FUNC;

    // ugh, the asm syntax is beyond me... by calling another func we are at least (guaranteed?) to get this value in r0.
    // at least thats what the calling convention says.
    volatile uint32_t* schedsp = &scheduler_stack[sizeof(scheduler_stack) / sizeof(scheduler_stack[0])];
    yield1(schedsp);

    return;
}

// One extra step for calling coro's entry point to make sure there's a yield_and_exit() when it returns.
static void entry_point_wrapper(coroutinefp_t func, int param)
{
    PROFILE_THIS_FUNC;

    uint32_t rv = func(param);

    // will never return here!
    yield_and_exit(rv);

    // but if we do by accident somehow...
    __breakpoint();
}

static bool is_live(CoroutineHeader* storage, int stacksize)
{
    PROFILE_THIS_FUNC;

    Coroutine<>*    ptrhelper = (Coroutine<>*) storage;

    // stack pointer should point to somewhere within the stack.
    bool is_below_top    = ptrhelper->sp > &ptrhelper->stack[0];
    bool is_above_bottom = ptrhelper->sp < &ptrhelper->stack[stacksize];

    return is_below_top && is_above_bottom;
}

#if PICO_USE_STACK_GUARDS
static void uninstall_stack_guard(void* stacktop)
{
    PROFILE_THIS_FUNC;

    static const int numregions = 8;
    const uint32_t   regionaddr = (uint32_t) stacktop & M0PLUS_MPU_RBAR_ADDR_BITS;

    // find the region that we may have configured for that address.
    for (int i = 0; i < numregions; ++i)
    {
        mpu_hw->rnr = i;
        // is region in use?
        if (mpu_hw->rasr & M0PLUS_MPU_RASR_ENABLE_BITS)
        {
            // we only need to match on region address, really.
            // we specifically do not try to do funny games with multiple subregions.
            if (regionaddr == (mpu_hw->rbar & M0PLUS_MPU_RBAR_ADDR_BITS))
            {
                // disable
                mpu_hw->rasr = 0;
                break;
            }
        }
    }
}

static void install_stack_guard(void* stacktop)
{
    PROFILE_THIS_FUNC;

#ifndef NDEBUG
    // the pico cpu has 8 mpu regions.
    const int numregions = (mpu_hw->type & M0PLUS_MPU_TYPE_DREGION_BITS) >> M0PLUS_MPU_TYPE_DREGION_LSB;
    assert(numregions >= 8);
#else
    static const int numregions = 8;
#endif

    // find us an unused region.
    // the last region will have been used by the pico-sdk to setup the mainflow stack guard, see pico-sdk/src/rp2_common/pico_runtime/runtime.c
    for (int i = 0; i < numregions; ++i)
    {
        mpu_hw->rnr = i;
        // region already in use?
        if (mpu_hw->rasr & M0PLUS_MPU_RASR_ENABLE_BITS)
            continue;

        // region is free, use it!

        // all our alignment pragma and stuff should have made sure of this.
        // stack address should be aligned to 32 byte or more.
        assert(((uint32_t) stacktop & 0x01F) == 0);

        // the region is addressed in chunks of 256 bytes.
        // NOTE: we make no attempt at trying to minimise region use by squeezing multiple subregions into one.
        const uint32_t    regionaddr = (uint32_t) stacktop & M0PLUS_MPU_RBAR_ADDR_BITS;
        // each region has chunks of 32 bytes for which the access permissions can be turned on or off.
        // find out which 32-byte-chunk the requested address falls into.
        const uint32_t    subregdisable = 0xFF ^ (1 << (((uint32_t) stacktop >> 5) & 0x07));

        mpu_hw->rbar = regionaddr | 0 | 0;  // set addr but none of the other fields.
        mpu_hw->rasr = 
            M0PLUS_MPU_RASR_ENABLE_BITS |       // enable
            (7 << M0PLUS_MPU_RASR_SIZE_LSB) |   // size of region is 2^(7+1) = 256 bytes, the minimum.
            (subregdisable << M0PLUS_MPU_RASR_SRD_LSB) |
            0x10000000;       // attributes: no read/write access at all, and no instruction fetch.

        break;
    }
}
#endif // if PICO_USE_STACK_GUARDS

void yield_and_start_ex(coroutinefp_t func, int param, CoroutineHeader* storage, int stacksize)
{
    PROFILE_THIS_FUNC;

    if (is_live(storage, stacksize))
    {
        yield();
        return;
    }

    bool is_first_coro = false;
    if (!mainflow.sp)
    {
        is_first_coro = true;
        ll_init_list(&ready2run);
        ll_init_list(&waiting4timer);
        critical_section_init(&lock);

        soonesttime2wake = at_the_end_of_time;
        soonestalarmid = 0;

#ifndef NDEBUG
        for (unsigned int i = 0; i < sizeof(scheduler_stack) / sizeof(scheduler_stack[i]); ++i)
            scheduler_stack[i] = 0xdeadbeef;
#endif

#if PICO_USE_STACK_GUARDS
        // we basically loose 32 bytes of otherwise usable stack space.
        install_stack_guard((void*) &scheduler_stack[0]);
#endif

#if PICORO_TRACK_EXECUTION_TIME
        headrunningsince = get_absolute_time();
#endif

        // i need mainflow in ready2run.head so that schedule_next() does the right thing.
        // remember: head is currently executing.
        ll_push_back(&ready2run, &mainflow.llentry);
        // mainflow.sp will be updated by yield().

        // run this pseudo coro only once (right now).
        mainflow.flags |= FLAGS_DO_NOT_RESCHEDULE;
        // cannot wait for mainflow to finish running, it'll always outlive any and all coros!
        mainflow.waitable.semaphore = 0x7F;
    }

    Coroutine<>*    ptrhelper = (Coroutine<>*) storage;
    assert(((int32_t) &ptrhelper->stack[0]) % 8 == 0);

#ifndef NDEBUG  // for debugging
    for (int i = 0; i < stacksize; ++i)
        ptrhelper->stack[i] = 0xdeadbeef;
#endif

    storage->waitable.waitchain = NULL;
    storage->waitable.semaphore = 0;
    storage->flags = 0;
    storage->sleepcount = 0;
#if PICORO_TRACK_EXECUTION_TIME
    storage->timespentexecuting = 0;
#endif
    storage->stacksize = stacksize;
    const int bottom_element = stacksize;
    // points to *past* the last element!
    storage->sp = &ptrhelper->stack[bottom_element];
    // "push" some values onto the stack.
    // this needs to match what yield() does!
    *--storage->sp = (uint32_t) entry_point_wrapper;
    *--storage->sp = 0;
    *--storage->sp = 0;
    *--storage->sp = 0;
    *--storage->sp = 0;
    *--storage->sp = 0;
    *--storage->sp = 0;
    *--storage->sp = param;             // r1
    *--storage->sp = (uint32_t) func;   // r0
    *--storage->sp = 0;
    *--storage->sp = 0;
    *--storage->sp = 0;
    *--storage->sp = 0;
    *--storage->sp = 0;

    critical_section_enter_blocking(&lock);
#if PICO_USE_STACK_GUARDS
    // not sure whether we need to do this under lock.
    install_stack_guard((void*) &ptrhelper->stack[0]);
#endif
    ll_push_back(&ready2run, &storage->llentry);
    critical_section_exit(&lock);

    yield();

    if (is_first_coro)
    {
#if PICO_USE_STACK_GUARDS
        uninstall_stack_guard((void*) &scheduler_stack[0]);
#endif
        // coroutines don't end up here.
        // but the mainflow does... and we'll need to cleanup.
        critical_section_deinit(&lock);
    }
}

void yield_and_exit(uint32_t exitcode)
{
    PROFILE_THIS_FUNC;

    critical_section_enter_blocking(&lock);
    {
        struct CoroutineHeader* self = LL_ACCESS(self, llentry, ll_peek_head(&ready2run));
        self->flags |= FLAGS_DO_NOT_RESCHEDULE;
        self->exitcode = exitcode;
    }
    critical_section_exit(&lock);

    yield();
}

void yield_and_wait4time(absolute_time_t until)
{
    PROFILE_THIS_FUNC;

    critical_section_enter_blocking(&lock);
    {
        struct CoroutineHeader* self = LL_ACCESS(self, llentry, ll_peek_head(&ready2run));
        self->sleepcount++;
        self->wakeuptime = until;
    }
    critical_section_exit(&lock);

    yield();
}

void yield_and_wait4wakeup()
{
    PROFILE_THIS_FUNC;

    yield_and_wait4time(at_the_end_of_time);
}

void yield_and_wait4signal(Waitable* other)
{
    PROFILE_THIS_FUNC;

#ifndef NDEBUG  // for debugging
    struct CoroutineHeader* oldself = 0;
    critical_section_enter_blocking(&lock);
    {
        oldself = LL_ACCESS(oldself, llentry, ll_peek_head(&ready2run));

        // FIXME: for the time being, only 1 coro can wait. so better check that there isnt one waiting already.
        assert(other->waitchain == NULL);
    }
    critical_section_exit(&lock);
#endif

    while (true)
    {
        critical_section_enter_blocking(&lock);
        {
            if (other->semaphore > 0)
            {
                // see signal(). this is currently handled by signal() and should always be the case.
                assert(other->waitchain == NULL);

                other->semaphore--;
                critical_section_exit(&lock);
                break;
            }
            struct CoroutineHeader* self = LL_ACCESS(self, llentry, ll_peek_head(&ready2run));
            assert(self == oldself);

            // there is a "race" condition: coro1 and coro2 both wait for coro3 to exit, coro1 wakes first and rescheds coro3, then could happen that coro2 never wakes up.
            other->waitchain = self;        // FIXME: do proper chain stuff!
        }
        critical_section_exit(&lock);

        // FIXME: if we wanted to be stingy on header space we could recycle llentry as a wait chain.
        //        at the cost of making management of waiting4timer more complicated.
        yield_and_wait4wakeup();
    }
}

/** @internal */
static void wakeup_locked(CoroutineHeader* coro)
{
    PROFILE_THIS_FUNC;

#ifndef NDEBUG  // for debugging
    // may not be true? with old stray timer alarms.
    assert(is_live(coro, coro->stacksize));
#endif

    // beware: wakeup() might have been called too soon, before schedule_next() has had a chance to put it on waiting4timer.
    // e.g. from an irq handler. that actually happens quite often.
    coro->sleepcount--;
    coro->wakeuptime = nil_time;
    // ll_remove() behaves fine if coro is not actually on waiting4timer (yet), it just does nothing.
    ll_remove(&waiting4timer, &coro->llentry);

    // the current coro might not have had a chance yet to call yield_and_wait4wakeup() and is thus still running.
    if (ll_peek_head(&ready2run) != &coro->llentry)
    {
        // FIXME: this could make coro the head of the queue! which to schedule_next() means it's running.
        //        i dont know yet what that will mean...
        ll_push_back(&ready2run, &coro->llentry);
    }
}

void wakeup(CoroutineHeader* coro)
{
    PROFILE_THIS_FUNC;

    // likely to be called from interrupt/exception handler!

    critical_section_enter_blocking(&lock);
    wakeup_locked(coro);
    critical_section_exit(&lock);

    // never ever call yield() here!
}

void signal(Waitable* waitable)
{
    PROFILE_THIS_FUNC;

    critical_section_enter_blocking(&lock);
    waitable->semaphore++;
    if (waitable->waitchain != NULL)
    {
        // FIXME: what happens if the same waitable is signaled twice?
        //        semaphore count goes up, sure.
        //        but we'd also wakeup() the waiting coro twice.
        //        at face value that should be fine.
        //        but our impl for the runqueue might choke...
        // FIXME: i suppose the question is: who manages waitchain, signal() or yield_and_wait4other()...
        wakeup_locked(waitable->waitchain);

        // ...putting this here makes things easy, for now. i think.
        waitable->waitchain = NULL; // FIXME: do proper chain stuff!
    }
    critical_section_exit(&lock);
}

bool check_debugger_attached()
{
    PROFILE_THIS_FUNC;

    if (isdebuggerattached)
        return true;

    // we don't get direct access to the swd pins but we can observe what the debug core responds!
    // so when there's a debugger attached it's likely that the cpu responds something and we can see this bit change value over time.
    static const int initialswd = syscfg_hw->dbgforce & SYSCFG_DBGFORCE_PROC0_SWDO_BITS;
    int swd = syscfg_hw->dbgforce & SYSCFG_DBGFORCE_PROC0_SWDO_BITS;
    if (swd != initialswd)
        isdebuggerattached = true;

    return isdebuggerattached;
}
