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

struct ChargerStatus {
    bool valid = false;

    // 原始电平，便于调试。
    bool pg_level = true;
    bool chg_level = true;

    // 解释后的状态。
    bool external_power_good = false;
    bool charging = false;
};

struct BatteryUiStatus {
    bool valid = false;

    uint32_t mv_battery = 0;
    uint32_t mv_adc = 0;
    uint16_t raw = 0;

    uint8_t percent = 0;

    bool external_power_good = false;
    bool charging = false;

    uint32_t updated_ms = 0;
};

// 后台电池状态缓存：
// tick 内部会控制采样频率，UI 只读取缓存，不直接采样 ADC。
void board_hw_battery_status_tick();
BatteryUiStatus board_hw_get_battery_status_cached();

ChargerStatus board_hw_read_charger_status();

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

/**
 * @brief 释放电源保持脚，触发整机断电。
 *
 * 前提：硬件电源自锁由 POWER_CTRL 维持。
 * 调用后通常不会返回到正常运行状态。
 */
void board_hw_power_off();

/** 模拟按一下蓝牙 SW，默认低脉冲 200ms。 */
bool board_hw_pulse_bt_switch(uint32_t pulse_ms = 200);

bool board_hw_set_amp_mute(bool enabled);
bool board_hw_get_amp_mute();

bool board_hw_set_amp_shutdown(bool enabled);
bool board_hw_get_amp_shutdown();

// ============================================================
// TC118S / 电磁铁脉冲驱动
// ============================================================

enum class SolenoidDirection : uint8_t {
    A = 0,  // SOL_CTRL_A=1, SOL_CTRL_B=0
    B = 1,  // SOL_CTRL_A=0, SOL_CTRL_B=1
};

/**
 * @brief 初始化 TC118S 控制脚，默认停止态。
 *
 * SOL_CTRL_A = MCP23017 GPB0
 * SOL_CTRL_B = MCP23017 GPB1
 */
bool board_hw_solenoid_begin();

/** @brief 立即停止电磁铁输出，A/B 全部拉低。 */
bool board_hw_solenoid_stop();

/** @brief 触发一次 A 方向短脉冲，默认 80ms，到时自动停止。 */
bool board_hw_solenoid_pulse_a(uint32_t pulse_ms = 80);

/** @brief 触发一次 B 方向短脉冲，默认 80ms，到时自动停止。 */
bool board_hw_solenoid_pulse_b(uint32_t pulse_ms = 80);

/** @brief 按上一次方向自动翻转，触发一次短脉冲。 */
bool board_hw_solenoid_flip(uint32_t pulse_ms = 80);

/** @brief 主循环里调用，用来在脉冲到时后自动断电。 */
void board_hw_solenoid_tick();

/** @brief 当前是否处在电磁铁脉冲输出期间。 */
bool board_hw_solenoid_is_busy();

void board_hw_debug_dump();