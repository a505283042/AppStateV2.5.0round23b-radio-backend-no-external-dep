#pragma once

#include <stdint.h>

void i2c_bus_lock();
void i2c_bus_unlock();

void i2c_bus_set_ready(bool ready);
bool i2c_bus_is_ready();
bool i2c_bus_io_allowed();

// MCP23017/RTC 等关键设备上报总线事务结果。
// 连续失败达到阈值后只标记恢复请求，真正恢复在主循环 service 中执行。
void i2c_bus_note_critical_result(bool success, uint8_t error_code);

// 在主循环中高频调用。返回 true 表示本轮完成过一次总线恢复。
bool i2c_bus_service();

// 每次成功恢复后递增，设备驱动可据此重新下发寄存器配置。
uint32_t i2c_bus_generation();
