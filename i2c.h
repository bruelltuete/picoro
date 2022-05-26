#pragma once
#include "coroutine.h"
#include "hardware/i2c.h"


#define I2C_STOP    (I2C_IC_DATA_CMD_STOP_VALUE_ENABLE << I2C_IC_DATA_CMD_STOP_LSB)
#define I2C_READ    (I2C_IC_DATA_CMD_CMD_VALUE_READ << I2C_IC_DATA_CMD_CMD_LSB)
#define I2C_WRITE   (I2C_IC_DATA_CMD_CMD_VALUE_WRITE << I2C_IC_DATA_CMD_CMD_LSB)
#define I2C_RESTART (I2C_IC_DATA_CMD_RESTART_VALUE_ENABLE << I2C_IC_DATA_CMD_RESTART_LSB)


// can yield_and_wait on the return value.
// it's a mistake to specify a results buffer without including read commands! will cause a deadlock!
// does not itself yield internally, unless internal ringbuffer is full.
extern Waitable* queue_cmds(i2c_inst_t* i2c, int8_t address, int numcmds, uint16_t* cmds, int numresults, uint8_t* results, bool* success);

// FIXME: i need an explicit init()/deinit() so that the main app can shut everything down.


// can return NULL if there is no driver yet for that i2c instance!
// maybe only useful for debugging.
extern const CoroutineHeader* get_driver_coro(i2c_inst_t* i2c);

// tells the drivers (both!) to stop, usually after their current commands have drained.
extern void stop_i2c_driver_async();
