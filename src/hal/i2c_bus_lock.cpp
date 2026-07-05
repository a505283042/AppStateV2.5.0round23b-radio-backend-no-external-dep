#include "hal/i2c_bus_lock.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {

StaticSemaphore_t s_i2c_bus_mu_buf;
SemaphoreHandle_t s_i2c_bus_mu = nullptr;
volatile bool s_i2c_bus_ready = false;

SemaphoreHandle_t i2c_bus_mutex()
{
    if (!s_i2c_bus_mu) {
        s_i2c_bus_mu = xSemaphoreCreateRecursiveMutexStatic(&s_i2c_bus_mu_buf);
    }
    return s_i2c_bus_mu;
}

}

void i2c_bus_lock()
{
    SemaphoreHandle_t mu = i2c_bus_mutex();
    if (mu) {
        xSemaphoreTakeRecursive(mu, portMAX_DELAY);
    }
}

void i2c_bus_unlock()
{
    if (s_i2c_bus_mu) {
        xSemaphoreGiveRecursive(s_i2c_bus_mu);
    }
}

void i2c_bus_set_ready(bool ready)
{
    s_i2c_bus_ready = ready;
}

bool i2c_bus_is_ready()
{
    return s_i2c_bus_ready;
}