#pragma once

// mpu-protects all accesses to unused flash, as determined by linker at build time.
extern bool block_unused_flash();
