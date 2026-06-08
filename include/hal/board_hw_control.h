#pragma once

#include <Arduino.h>
#include <stdint.h>

/**
 * @brief PCB1 板载硬件控制验证层。
 *
 * 这一层只负责最基础的硬件读写：
 * - BAT_ADC 电池电压读取
 * - BT_PWR_EN 蓝牙电源控制
 * - MUTE_EN 功放静音控制
 * - SHDN_EN 功放关断控制
 *
 * 注意：
 * 1. 第一版不保存 NVS。
 * 2. MUTE_EN / SHDN_EN / BT_PWR_EN 的有效电平先按“高=使能”做验证。
 * 3. 如果实测逻辑相反，只改这里，不改菜单层。
 */

struct BatterySample {
    uint16_t raw = 0;
    uint32_t mv_adc = 0;      // ESP32 ADC 管脚测到的毫伏
    uint32_t mv_battery = 0;  // 按分压比例估算后的电池毫伏
};

bool board_hw_control_begin();

BatterySample board_hw_read_battery();

bool board_hw_set_bt_power(bool enabled);
bool board_hw_get_bt_power();

bool board_hw_set_bt_wakeup(bool enabled);
bool board_hw_get_bt_wakeup();

bool board_hw_set_bt_switch(bool level);
bool board_hw_get_bt_switch();

bool board_hw_set_backlight(bool enabled);
bool board_hw_get_backlight();

/** 模拟按一下蓝牙 SW，默认低脉冲 200ms。 */
bool board_hw_pulse_bt_switch(uint32_t pulse_ms = 200);

bool board_hw_set_amp_mute(bool enabled);
bool board_hw_get_amp_mute();

bool board_hw_set_amp_shutdown(bool enabled);
bool board_hw_get_amp_shutdown();

void board_hw_debug_dump();