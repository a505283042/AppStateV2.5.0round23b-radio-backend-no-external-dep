#pragma once

#include <Arduino.h>
#include <stdint.h>

/**
 * @brief PCB1 板载硬件控制验证层。
 *
 * 这一层只负责最基础的硬件读写：
 * - BQ27441 电池电量计读取
 * - BT_PWR_EN 蓝牙电源控制
 * - BT_MODE_CTRL 蓝牙发射/接收模式控制
 * - BT_LINK 蓝牙连接状态读取
 * - MUTE_EN 功放静音控制
 * - SHDN_EN 功放关断控制
 *
 * 注意：
 * 1. 第一版不保存 NVS。
 * 2. MUTE_EN / SHDN_EN / BT_PWR_EN 的有效电平先按"高=使能"做验证。
 * 3. 如果实测逻辑相反，只改这里，不改菜单层。
 */

struct BatterySample {
    bool valid = false;

    uint16_t raw = 0;
    uint32_t mv_adc = 0;
    uint32_t mv_battery = 0;

    uint8_t soc_percent = 0;
    int16_t average_current_ma = 0;
    uint16_t remaining_capacity_mah = 0;
    uint16_t full_charge_capacity_mah = 0;
    uint16_t design_capacity_mah = 0;
    uint16_t flags = 0;
    uint16_t state_of_health_percent = 0;
    bool gpout_level = true;
};

struct ChargerStatus {
    bool valid = false;

    bool pg_level = true;
    bool chg_level = true;

    bool external_power_good = false;
    bool charging = false;
};

enum class BatteryRuntimeEstimateState : uint8_t {
    Unavailable = 0,
    Charging,
    ExternalPower,
    LowCurrent,
    Stabilizing,
    Ready,
};

struct BatteryUiStatus {
    bool valid = false;

    uint32_t mv_battery = 0;
    uint32_t mv_adc = 0;
    uint16_t raw = 0;

    uint8_t percent = 0;
    int16_t average_current_ma = 0;
    uint16_t remaining_capacity_mah = 0;
    uint16_t full_charge_capacity_mah = 0;
    uint16_t design_capacity_mah = 0;
    uint16_t flags = 0;
    uint16_t state_of_health_percent = 0;
    bool gpout_level = true;

    bool external_power_good = false;
    bool charging = false;

    BatteryRuntimeEstimateState runtime_estimate_state =
        BatteryRuntimeEstimateState::Unavailable;
    uint16_t estimated_discharge_current_ma = 0;
    uint32_t estimated_runtime_minutes = 0;
    uint32_t estimate_stable_ms = 0;
    uint32_t estimate_updated_ms = 0;

    uint32_t updated_ms = 0;
};

// 主循环高频维护：处理 I2C 总线恢复和 MCP23017 重新初始化。
void board_hw_i2c_service();

void board_hw_battery_status_tick();
BatteryUiStatus board_hw_get_battery_status_cached();
const char* board_hw_battery_runtime_state_label(BatteryRuntimeEstimateState state);
void board_hw_battery_estimate_notify_load_change();

enum class BatteryShutdownReason : uint8_t {
    None = 0,
    SocFinal,
    SocCritical,
    VoltageCritical,
    GpoutLow,
};

BatteryShutdownReason board_hw_battery_shutdown_reason();
const char* board_hw_battery_shutdown_reason_label(BatteryShutdownReason reason);

ChargerStatus board_hw_read_charger_status();

bool board_hw_control_begin();

BatterySample board_hw_read_battery();

bool board_hw_set_bt_power(bool enabled);
bool board_hw_get_bt_power();

bool board_hw_set_bt_mode(bool transmit);
bool board_hw_get_bt_mode();

bool board_hw_read_bt_link(bool* linked);
bool board_hw_is_bt_linked();

bool board_hw_set_bt_switch(bool level);
bool board_hw_get_bt_switch();

bool board_hw_set_backlight(bool enabled);
bool board_hw_get_backlight();

void board_hw_power_off();

bool board_hw_pulse_bt_switch(uint32_t pulse_ms = 200);

bool board_hw_set_amp_mute(bool enabled);
bool board_hw_get_amp_mute();

bool board_hw_set_amp_shutdown(bool enabled);
bool board_hw_get_amp_shutdown();

enum class SolenoidDirection : uint8_t {
    A = 0,
    B = 1,
};

bool board_hw_solenoid_begin();

bool board_hw_solenoid_stop();

bool board_hw_solenoid_pulse_a(uint32_t pulse_ms = 150);

bool board_hw_solenoid_pulse_b(uint32_t pulse_ms = 150);

bool board_hw_solenoid_flip(uint32_t pulse_ms = 150);

void board_hw_solenoid_tick();

bool board_hw_solenoid_is_busy();

void board_hw_debug_dump();
