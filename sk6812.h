#pragma once
#include "coroutine.h"
#include "hardware/pio.h"


/**
 * @brief Stores "pixel" values for the LED strip.
 * 
 */
struct SK6812Framebuffer
{
    PIO     pio;
    int     sm;
    int     dmachannel;

    /**
     * The sum of all the pixel values. Can be used to estimate power consumption.
     * A good ballpark guestimate is 0.5mA per LED base current, ie no light.
     * And around 4.5mA per channel per LED at full brightness.
     * Ex: count_of(grbx_pixels) * 0.5 + sum * (4.5 / 255)
     */
    uint32_t        sum;

    // FIXME: to template or not to template...
    uint32_t    grbx_pixels[166];
};


extern bool init_sk6812(SK6812Framebuffer* fb, PIO pio, int datapin);

/**
 * @brief Lights up a strip of SK6812 LEDs according to the "framebuffer".
 * 
 * @param pio 
 * @return Waitable* signaled when the LEDs have been updated
 */
extern Waitable* update_sk6812ledstrip(SK6812Framebuffer* fb);
