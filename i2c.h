#pragma once
#include "coroutine.h"
#include "hardware/i2c.h"



// can yield_and_wait on the return value.
// it's a mistake to specify a results buffer without including read commands! will cause a deadlock!
// does not itself yield internally, unless internal ringbuffer is full.
extern Waitable* queue_cmds(i2c_inst_t* i2c, int8_t address, int numcmds, uint16_t* cmds, int numresults, uint8_t* results);

// FIXME: i need an explicit init()/deinit() so that the main app and shut everything down.
