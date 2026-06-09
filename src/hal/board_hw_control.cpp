#include "hal/board_hw_control.h"

#include <Arduino.h>
#include <driver/gpio.h>

#include "board/board_pins.h"
#include "board/board_pins_pcb1_mcp23017.h"
#include "hal/mcp23017_u3.h"
#include "utils/log.h"

namespace {

// 高阻分压输入，采样次数稍微多一点，降低 ESP32 ADC 抖动。
static constexpr uint8_t BATTERY_ADC_SAMPLE_COUNT = 16;
static constexpr uint16_t BATTERY_ADC_SETTLE_US = 300;

// EMA 平滑比例：
// 新值占 1/4，旧值占 3/4。
// 电池电压变化慢，这样显示更稳。
static constexpr uint32_t BATTERY_EMA_NEW_NUM = 1;
static constexpr uint32_t BATTERY_EMA_DEN = 4;

// 电池采样校准：
// 实测 BAT+ = 4.11V，BAT_ADC = 1.45V，菜单 ADC = 1.53V。
// 先把 analogReadMilliVolts() 的 ADC 电压校准到万用表读数。
static constexpr uint32_t BATTERY_ADC_CAL_NUM = 1450;
static constexpr uint32_t BATTERY_ADC_CAL_DEN = 1530;

// 当前板子实测分压倍率：BAT+ / BAT_ADC = 4110 / 1450 ≈ 2.834
static constexpr uint32_t BATTERY_DIVIDER_CAL_NUM = 4110;
static constexpr uint32_t BATTERY_DIVIDER_CAL_DEN = 1450;

// 电池平滑滤波状态
static bool s_battery_filter_ready = false;
static uint32_t s_battery_filtered_raw = 0;
static uint32_t s_battery_filtered_mv_adc = 0;
static uint32_t s_battery_filtered_mv_battery = 0;

static void configure_battery_adc_input()
{
    // BAT_ADC 是高阻分压输入，必须保持高阻输入状态。
    // 关闭内部上下拉，避免 GPIO1 把分压点拉偏。
    pinMode(PIN_BAT_ADC, INPUT);

#if defined(ARDUINO_ARCH_ESP32)
    gpio_num_t gpio = static_cast<gpio_num_t>(PIN_BAT_ADC);
    gpio_set_direction(gpio, GPIO_MODE_INPUT);
    gpio_pullup_dis(gpio);
    gpio_pulldown_dis(gpio);

    // BAT_ADC 理论满电约 1.4V，使用 11dB 衰减更安全。
    analogSetPinAttenuation(PIN_BAT_ADC, ADC_11db);
#endif
}

static uint32_t apply_ema_filter(uint32_t old_value, uint32_t new_value)
{
    return ((old_value * (BATTERY_EMA_DEN - BATTERY_EMA_NEW_NUM)) +
            (new_value * BATTERY_EMA_NEW_NUM)) /
           BATTERY_EMA_DEN;
}

static BatterySample apply_battery_filter(const BatterySample& sample)
{
    if (!s_battery_filter_ready) {
        s_battery_filtered_raw = sample.raw;
        s_battery_filtered_mv_adc = sample.mv_adc;
        s_battery_filtered_mv_battery = sample.mv_battery;
        s_battery_filter_ready = true;
    } else {
        s_battery_filtered_raw = apply_ema_filter(s_battery_filtered_raw, sample.raw);
        s_battery_filtered_mv_adc = apply_ema_filter(s_battery_filtered_mv_adc, sample.mv_adc);
        s_battery_filtered_mv_battery = apply_ema_filter(s_battery_filtered_mv_battery, sample.mv_battery);
    }

    BatterySample out = sample;
    out.raw = static_cast<uint16_t>(s_battery_filtered_raw);
    out.mv_adc = s_battery_filtered_mv_adc;
    out.mv_battery = s_battery_filtered_mv_battery;
    return out;
}

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
bool s_amp_mute_enabled = true;
bool s_amp_shutdown_enabled = true;
bool s_backlight_enabled = true;

bool level_from_enabled(bool enabled, bool active_level)
{
    return enabled ? active_level : !active_level;
}

}  // namespace

bool board_hw_control_begin()
{
    configure_battery_adc_input();

    s_ready = true;

    // 安全默认：
    // 蓝牙关闭；
    // 功放保持“静音 + 关断”，等真正播放前再打开，减少开机爆破音。
    board_hw_set_bt_power(false);
    board_hw_set_bt_wakeup(false);
    board_hw_set_bt_switch(false);
    board_hw_set_amp_mute(true);
    board_hw_set_amp_shutdown(true);

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

    configure_battery_adc_input();

    // 高阻分压 + ADC 采样电容，第一次读数容易偏。
    // 先丢弃两次，再进入正式采样。
    (void)analogRead(PIN_BAT_ADC);
    delayMicroseconds(BATTERY_ADC_SETTLE_US);
    (void)analogRead(PIN_BAT_ADC);
    delayMicroseconds(BATTERY_ADC_SETTLE_US);

    uint32_t raw_sum = 0;
    uint32_t mv_sum = 0;

    uint32_t raw_min = 0xFFFFFFFFu;
    uint32_t raw_max = 0;
    uint32_t mv_min = 0xFFFFFFFFu;
    uint32_t mv_max = 0;

    for (uint8_t i = 0; i < BATTERY_ADC_SAMPLE_COUNT; ++i) {
        const uint32_t raw = static_cast<uint32_t>(analogRead(PIN_BAT_ADC));

#if defined(ARDUINO_ARCH_ESP32)
        const uint32_t mv = static_cast<uint32_t>(analogReadMilliVolts(PIN_BAT_ADC));
#else
        const uint32_t mv = 0;
#endif

        raw_sum += raw;
        mv_sum += mv;

        if (raw < raw_min) raw_min = raw;
        if (raw > raw_max) raw_max = raw;
        if (mv < mv_min) mv_min = mv;
        if (mv > mv_max) mv_max = mv;

        delayMicroseconds(BATTERY_ADC_SETTLE_US);
    }

    // 去掉一个最大值和一个最小值，减少偶发尖峰。
    static constexpr uint8_t EFFECTIVE_SAMPLE_COUNT = BATTERY_ADC_SAMPLE_COUNT - 2;

    const uint32_t raw_avg = (raw_sum - raw_min - raw_max) / EFFECTIVE_SAMPLE_COUNT;
    const uint32_t mv_adc_raw = (mv_sum - mv_min - mv_max) / EFFECTIVE_SAMPLE_COUNT;

    s.raw = static_cast<uint16_t>(raw_avg);

#if defined(ARDUINO_ARCH_ESP32)
    // ESP32 ADC 软件读数校准到万用表实测 BAT_ADC。
    s.mv_adc = static_cast<uint32_t>(
        (static_cast<uint64_t>(mv_adc_raw) * BATTERY_ADC_CAL_NUM) /
        BATTERY_ADC_CAL_DEN
    );
#else
    s.mv_adc = 0;
#endif

    // 按当前板子实测分压倍率计算电池电压。
    s.mv_battery = static_cast<uint32_t>(
        (static_cast<uint64_t>(s.mv_adc) * BATTERY_DIVIDER_CAL_NUM) /
        BATTERY_DIVIDER_CAL_DEN
    );

    return apply_battery_filter(s);
}

ChargerStatus board_hw_read_charger_status()
{
    ChargerStatus s{};

    if (!mcp23017_u3_is_ready()) {
        return s;
    }

    bool pg_level = true;
    bool chg_level = true;

    const bool pg_ok = mcp23017_u3_read_b_bit(board::MCP_B_PG, &pg_level);
    const bool chg_ok = mcp23017_u3_read_b_bit(board::MCP_B_CHG_STAT, &chg_level);

    s.valid = pg_ok && chg_ok;
    s.pg_level = pg_level;
    s.chg_level = chg_level;

    // BQ25606 /PG 是低有效，PG 低表示外部输入有效。
    s.external_power_good = !pg_level;

    // 注意：板子上的 CHG_STAT 不是 BQ25606 STAT 原始电平。
    // BQ STAT 经过 Q4 反相后接到 MCP23017。
    // 因此 MCP 读到 CHG_STAT = 高，才表示 BQ STAT = 低，也就是正在充电。
    s.charging = chg_level;

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

void board_hw_power_off()
{
    LOGI("[HWCTRL] power off: release POWER_CTRL GPIO%d", PIN_POWER_CTRL);

    pinMode(PIN_POWER_CTRL, OUTPUT);
    digitalWrite(PIN_POWER_CTRL, LOW);
}