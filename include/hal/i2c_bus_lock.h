#pragma once

#include <stdint.h>

void i2c_bus_lock();
void i2c_bus_unlock();

void i2c_bus_set_ready(bool ready);
bool i2c_bus_is_ready();