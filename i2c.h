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
#define I2C_START               (0)
#define I2C_STOP                ((uint16_t) (I2C_IC_DATA_CMD_STOP_VALUE_ENABLE << I2C_IC_DATA_CMD_STOP_LSB))
#define I2C_WRITE(value)        ((uint16_t) (((uint8_t) value) | (I2C_IC_DATA_CMD_CMD_VALUE_WRITE << I2C_IC_DATA_CMD_CMD_LSB)))
#define I2C_READ(ignored)       ((uint16_t) (I2C_IC_DATA_CMD_CMD_VALUE_READ << I2C_IC_DATA_CMD_CMD_LSB))
#define I2C_RESTART             ((uint16_t) (I2C_IC_DATA_CMD_RESTART_VALUE_ENABLE << I2C_IC_DATA_CMD_RESTART_LSB))


// can yield_and_wait on the return value.
// it's a mistake to specify a results buffer without including read commands! will cause a deadlock!
// does not itself yield internally, unless internal ringbuffer is full.
extern Waitable* queue_cmds(i2c_inst_t* i2c, int8_t address, int numcmds, const uint16_t* cmds, int numresults, uint8_t* results, bool* success);

// FIXME: i need an explicit init()/deinit() so that the main app can shut everything down.


// can return NULL if there is no driver yet for that i2c instance!
// maybe only useful for debugging.
extern const CoroutineHeader* get_driver_coro(i2c_inst_t* i2c);

// tells the drivers (both!) to stop, usually after their current commands have drained.
extern void stop_i2c_driver_async();
