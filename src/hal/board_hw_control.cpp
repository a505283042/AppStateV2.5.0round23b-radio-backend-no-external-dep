#include "hal/board_hw_control.h"

#include <Arduino.h>

#include "board/board_pins.h"
#include "board/board_pins_pcb1_mcp23017.h"
#include "hal/mcp23017_u3.h"
#include "utils/log.h"

namespace {

// BAT_ADC 分压：上拉 200K，下拉 100K
// Vadc = Vbat * 100 / (200 + 100) = Vbat / 3
// Vbat = Vadc * 3
static constexpr uint32_t BATTERY_DIVIDER_NUM = 3;
static constexpr uint32_t BATTERY_DIVIDER_DEN = 1;

// 当前先假设高电平为“开启/使能”。
// 如果实测相反，只改下面这三个常量。
static constexpr bool BT_PWR_ACTIVE_LEVEL = true;
// PAM8406：MUTE 低电平静音，高电平正常输出。
// 这里 enabled=true 表示“静音启用”，所以 active level 应该是 LOW。
static constexpr bool AMP_MUTE_ACTIVE_LEVEL = false;

// PAM8406：SHDN 低电平关断，高电平正常工作。
// 这里 enabled=true 表示“关断启用”，所以 active level 应该是 LOW。
static constexpr bool AMP_SHDN_ACTIVE_LEVEL = false;

// 先按常见做法：WKP 高电平唤醒。
// 如果实测相反，只改这里。
static constexpr bool BT_WKP_ACTIVE_LEVEL = true;

// SW 更像“按键脚”，常见是低有效按下。
static constexpr bool BT_SW_ACTIVE_LEVEL = false;

bool s_ready = false;
bool s_bt_power_enabled = false;
bool s_bt_wakeup_enabled = false;
bool s_bt_switch_level = !BT_SW_ACTIVE_LEVEL;
bool s_amp_mute_enabled = false;
bool s_amp_shutdown_enabled = false;
bool s_backlight_enabled = true;

bool level_from_enabled(bool enabled, bool active_level)
{
    return enabled ? active_level : !active_level;
}

}  // namespace

bool board_hw_control_begin()
{
    pinMode(PIN_BAT_ADC, INPUT);

#if defined(ARDUINO_ARCH_ESP32)
    // 3.3V 量程附近更合适。不同 Arduino-ESP32 版本函数可用性可能略有差异。
    analogSetPinAttenuation(PIN_BAT_ADC, ADC_11db);
#endif

    s_ready = true;

    // 第一版安全默认：
    // 蓝牙关闭、静音关闭、功放不关断。
    board_hw_set_bt_power(false);
    board_hw_set_bt_wakeup(false);
    board_hw_set_bt_switch(false);
    board_hw_set_amp_mute(false);
    board_hw_set_amp_shutdown(false);

    LOGI("[HWCTRL] begin ok BAT_ADC=%d BT_PWR=MCPB%d MUTE=MCPA%d SHDN=MCPA%d",
         PIN_BAT_ADC,
         board::MCP_B_BT_PWR_EN,
         board::MCP_A_MUTE_EN,
         board::MCP_A_SHDN_EN);

    return s_ready;
}

BatterySample board_hw_read_battery()
{
    BatterySample s{};

    s.raw = (uint16_t)analogRead(PIN_BAT_ADC);

#if defined(ARDUINO_ARCH_ESP32)
    s.mv_adc = (uint32_t)analogReadMilliVolts(PIN_BAT_ADC);
#else
    s.mv_adc = 0;
#endif

    s.mv_battery = (s.mv_adc * BATTERY_DIVIDER_NUM) / BATTERY_DIVIDER_DEN;
    return s;
}

bool board_hw_set_bt_power(bool enabled)
{
    if (!mcp23017_u3_is_ready()) return false;

    const bool level = level_from_enabled(enabled, BT_PWR_ACTIVE_LEVEL);
    if (!mcp23017_u3_set_b(board::MCP_B_BT_PWR_EN, level)) {
        return false;
    }

    s_bt_power_enabled = enabled;
    LOGI("[HWCTRL] BT power %s level=%d", enabled ? "ON" : "OFF", level ? 1 : 0);
    return true;
}

bool board_hw_get_bt_power()
{
    return s_bt_power_enabled;
}

bool board_hw_set_bt_wakeup(bool enabled)
{
    if (!mcp23017_u3_is_ready()) return false;

    const bool level = level_from_enabled(enabled, BT_WKP_ACTIVE_LEVEL);
    if (!mcp23017_u3_set_a(board::MCP_A_BT_WKP_CTRL, level)) {
        return false;
    }

    s_bt_wakeup_enabled = enabled;
    LOGI("[HWCTRL] BT wakeup %s level=%d", enabled ? "ON" : "OFF", level ? 1 : 0);
    return true;
}

bool board_hw_get_bt_wakeup()
{
    return s_bt_wakeup_enabled;
}

bool board_hw_set_bt_switch(bool pressed)
{
    if (!mcp23017_u3_is_ready()) return false;

    const bool level = level_from_enabled(pressed, BT_SW_ACTIVE_LEVEL);
    if (!mcp23017_u3_set_a(board::MCP_A_BT_SW_CTRL, level)) {
        return false;
    }

    s_bt_switch_level = level;
    LOGI("[HWCTRL] BT switch pressed=%d level=%d", pressed ? 1 : 0, level ? 1 : 0);
    return true;
}

bool board_hw_get_bt_switch()
{
    return s_bt_switch_level;
}

bool board_hw_set_backlight(bool enabled)
{
    if (!mcp23017_u3_is_ready()) {
        return false;
    }

    if (!mcp23017_u3_set_b(board::MCP_B_BLK, enabled)) {
        return false;
    }

    s_backlight_enabled = enabled;
    LOGI("[HWCTRL] backlight %s", enabled ? "ON" : "OFF");
    return true;
}

bool board_hw_get_backlight()
{
    return s_backlight_enabled;
}

bool board_hw_pulse_bt_switch(uint32_t pulse_ms)
{
    if (!board_hw_set_bt_switch(true)) return false;
    delay(pulse_ms);
    return board_hw_set_bt_switch(false);
}

bool board_hw_set_amp_mute(bool enabled)
{
    if (!mcp23017_u3_is_ready()) return false;

    const bool level = level_from_enabled(enabled, AMP_MUTE_ACTIVE_LEVEL);
    if (!mcp23017_u3_set_a(board::MCP_A_MUTE_EN, level)) {
        return false;
    }

    s_amp_mute_enabled = enabled;
    LOGI("[HWCTRL] AMP mute %s level=%d", enabled ? "ON" : "OFF", level ? 1 : 0);
    return true;
}

bool board_hw_get_amp_mute()
{
    return s_amp_mute_enabled;
}

bool board_hw_set_amp_shutdown(bool enabled)
{
    if (!mcp23017_u3_is_ready()) return false;

    const bool level = level_from_enabled(enabled, AMP_SHDN_ACTIVE_LEVEL);
    if (!mcp23017_u3_set_a(board::MCP_A_SHDN_EN, level)) {
        return false;
    }

    s_amp_shutdown_enabled = enabled;
    LOGI("[HWCTRL] AMP shutdown %s level=%d", enabled ? "ON" : "OFF", level ? 1 : 0);
    return true;
}

bool board_hw_get_amp_shutdown()
{
    return s_amp_shutdown_enabled;
}

void board_hw_debug_dump()
{
    const BatterySample bat = board_hw_read_battery();

    bool pg_level = true;
    bool chg_level = true;
    (void)mcp23017_u3_read_b_bit(board::MCP_B_PG, &pg_level);
    (void)mcp23017_u3_read_b_bit(board::MCP_B_CHG_STAT, &chg_level);

    LOGI("[HWCTRL] dump bat_raw=%u adc=%lumV bat=%lumV bt=%d mute=%d shdn=%d PG=%d CHG=%d",
         bat.raw,
         (unsigned long)bat.mv_adc,
         (unsigned long)bat.mv_battery,
         s_bt_power_enabled ? 1 : 0,
         s_amp_mute_enabled ? 1 : 0,
         s_amp_shutdown_enabled ? 1 : 0,
         pg_level ? 1 : 0,
         chg_level ? 1 : 0);
}