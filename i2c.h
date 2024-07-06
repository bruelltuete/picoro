#pragma once
#include "coroutine.h"
#include "hardware/i2c.h"


// define to place i2c driver functions in ram.
#ifndef PICORO_I2CDRV_IN_RAM
#define PICORO_I2CDRV_IN_RAM         0
#endif


/*

static const uint16_t    cmds[] = {
    I2C_START   | I2C_WRITE(123),
    I2C_RESTART | I2C_READ(123) | I2C_STOP
};

*/
// if gcc complains about narrowing conversions, this is prob a compiler bug.
constexpr uint16_t I2C_START = 0;
constexpr uint16_t I2C_STOP = I2C_IC_DATA_CMD_STOP_VALUE_ENABLE << I2C_IC_DATA_CMD_STOP_LSB;
constexpr uint16_t I2C_WRITE(uint8_t value) { return ((uint16_t) value) | (I2C_IC_DATA_CMD_CMD_VALUE_WRITE << I2C_IC_DATA_CMD_CMD_LSB); }
constexpr uint16_t I2C_READ(int ignored) { return I2C_IC_DATA_CMD_CMD_VALUE_READ << I2C_IC_DATA_CMD_CMD_LSB; }
constexpr uint16_t I2C_RESTART = I2C_IC_DATA_CMD_RESTART_VALUE_ENABLE << I2C_IC_DATA_CMD_RESTART_LSB;

// can yield_and_wait on the return value.
// it's a mistake to specify a results buffer without including read commands! will cause a deadlock!
// does not itself yield internally, unless internal ringbuffer is full.
extern Waitable* queue_cmds(i2c_inst_t* i2c, int8_t address, int numcmds, const uint16_t* cmds, int numresults, uint8_t* results, bool* success);

template <int C, int R>
Waitable* queue_cmds(i2c_inst_t* i2c, int8_t address, const uint16_t (& cmds)[C], uint8_t (& results)[R], bool* success)
{
    return queue_cmds(i2c, address, C, &cmds[0], R, &results[0], success);
}
template <int C>
Waitable* queue_cmds(i2c_inst_t* i2c, int8_t address, const uint16_t (& cmds)[C], uint8_t* results, bool* success)
{
    return queue_cmds(i2c, address, C, &cmds[0], 1, results, success);
}
template <int C>
Waitable* queue_cmds(i2c_inst_t* i2c, int8_t address, const uint16_t (& cmds)[C], bool* success)
{
    return queue_cmds(i2c, address, C, &cmds[0], 0, NULL, success);
}

// FIXME: i need an explicit init()/deinit() so that the main app can shut everything down.


// can return NULL if there is no driver yet for that i2c instance!
// maybe only useful for debugging.
extern const CoroutineHeader* get_driver_coro(i2c_inst_t* i2c);

// tells the drivers (both!) to stop, usually after their current commands have drained.
extern void stop_i2c_driver_async();
