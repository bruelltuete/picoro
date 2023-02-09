#pragma once
#include "pico/stdlib.h"


template <int L, int H>
struct ChargeHystereris
{
    bool        charging;

    ChargeHystereris()
        : charging(false)
    {
    }

    bool update(int32_t value)
    {
        if (value < L)
            return charging = true;
        if (value > H)
            return charging = false;

        return charging;
    }
};


#if !PICO_PRINTF_ALWAYS_INCLUDED
// if the above symbol is not defined then assert's printf does not work!
#endif
// copied from assert macro.
#define CHECK(__e) ((__e) ? (void)0 : __assert_func(__FILE__, __LINE__, __PRETTY_FUNCTION__, #__e))

static inline void chargehysteresis_unit_tests()
{
    ChargeHystereris<100, 200>  c;
    CHECK(c.charging == false);

    // below lower limit
    bool r = c.update(0);
    CHECK(r == true);
    CHECK(c.charging == true);

    // between low and high
    r = c.update(150);
    CHECK(r == true);
    CHECK(c.charging == true);

    r = c.update(190);
    CHECK(r == true);
    CHECK(c.charging == true);

    // above high
    r = c.update(250);
    CHECK(r == false);
    CHECK(c.charging == false);

    // still high but falling
    r = c.update(200);
    CHECK(r == false);
    CHECK(c.charging == false);

    r = c.update(150);
    CHECK(r == false);
    CHECK(c.charging == false);

    r = c.update(100);
    CHECK(r == false);
    CHECK(c.charging == false);

    // too low, charge again.
    r = c.update(50);
    CHECK(r == true);
    CHECK(c.charging == true);
}

#undef CHECK
