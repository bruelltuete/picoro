#include "mpu.h"
#include "hardware/structs/mpu.h"
#include <cassert>


#ifndef PICO_FLASH_SIZE_BYTES
#error
#endif

#if PICO_NO_FLASH
#error
#endif

int find_free_mpu_region()
{
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

        return i;
    }

    // no free ones
    return -1;
}

bool block_unused_flash()
{
    int region = find_free_mpu_region();
    if (region == -1)
        return false;

    mpu_hw->rnr = region;

    extern char __flash_binary_start;
    extern char __flash_binary_end;

    const char* topofusedflash = &__flash_binary_end;
    const char* topofallflash = ((const char*) XIP_BASE) + PICO_FLASH_SIZE_BYTES;

    assert((1 << 21) == PICO_FLASH_SIZE_BYTES);

    int subregionbits = 0;
    for (const char* p = &__flash_binary_start; p < topofusedflash; p += PICO_FLASH_SIZE_BYTES / 8)
    {
        subregionbits = (subregionbits << 1) | 1;
    }

    // remember region's base address has to be size-aligned!
    // so if we have 2 mb region, it needs to be aligned to 2mb... which is rubbish...
    const uint32_t    regionaddr = (uint32_t) XIP_BASE & M0PLUS_MPU_RBAR_ADDR_BITS;
    // so we just cover the whole of flash, and mask off the good code with subregiondisable
    // but that has only 256kb granularity, also rubbish.
    const uint32_t    subregdisable = subregionbits;

    mpu_hw->rbar = regionaddr | 0 | 0;  // set addr but none of the other fields.
    mpu_hw->rasr = 
        M0PLUS_MPU_RASR_ENABLE_BITS |       // enable
        ((21-1) << M0PLUS_MPU_RASR_SIZE_LSB) |   // size of region is 2^(x+1)
        (subregdisable << M0PLUS_MPU_RASR_SRD_LSB) |
        0x10000000;       // attributes: no read/write access at all, and no instruction fetch.

    return true;
}
