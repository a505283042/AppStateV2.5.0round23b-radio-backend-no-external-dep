#include "hal/board_hw_control.h"

#include <Arduino.h>
#include <driver/gpio.h>

#include "board/board_pins.h"
#include "board/board_pins_pcb1_mcp23017.h"
#include "hal/bq27441.h"
#include "hal/i2c_bus_lock.h"
#include "hal/mcp23017_u3.h"
#include "utils/log.h"

namespace {

// BQ27441DRZR-G1A 电量计：
// - I2C 地址 0x55，和 MCP23017 共用 Wire 总线。
// - 原 BAT_ADC / GPIO1 现在接 BQ27441 GPOUT，只做数字输入/告警检测，不再做 ADC 采样。
// - 电池电压、SOC、容量、电流全部从 BQ27441 标准命令读取。

// 电池 UI 缓存采样策略：
// 1. 上电后先立即采样；
// 2. 前几次每 3 秒采样一次；
// 3. 稳定后每 1 分钟采样一次。
// 4. PG/CHG 是充电芯片数字状态，单独每 1 秒刷新一次，插 USB 后闪电能尽快显示。
static constexpr uint32_t BATTERY_UI_BOOT_SAMPLE_INTERVAL_MS = 3000;
static constexpr uint32_t BATTERY_UI_STABLE_SAMPLE_INTERVAL_MS = 60UL * 1000UL;
static constexpr uint32_t BATTERY_RUNTIME_SAMPLE_INTERVAL_MS = 10UL * 1000UL;
static constexpr uint32_t BATTERY_RUNTIME_SETTLE_MS = 30UL * 1000UL;
static constexpr uint16_t BATTERY_RUNTIME_MIN_DISCHARGE_MA = 20;
static constexpr uint16_t BATTERY_RUNTIME_SHIFT_MIN_MA = 35;
static constexpr uint8_t BATTERY_RUNTIME_SHIFT_PERCENT = 30;
static constexpr uint32_t CHARGER_UI_SAMPLE_INTERVAL_MS = 1000;
static constexpr uint8_t BATTERY_UI_BOOT_SAMPLE_COUNT = 5;

static constexpr uint8_t BATTERY_SHUTDOWN_SOC_PERCENT = 5;
static constexpr uint8_t BATTERY_GPOUT_CONFIRM_SOC_PERCENT = 10;
static constexpr uint32_t BATTERY_SHUTDOWN_VOLTAGE_MV = 3400;
static constexpr uint32_t BATTERY_GPOUT_CONFIRM_VOLTAGE_MV = 3600;
static constexpr uint8_t BATTERY_SHUTDOWN_CONFIRM_COUNT = 2;
static constexpr uint32_t BATTERY_SHUTDOWN_CHECK_INTERVAL_MS = 5000;

static BatteryUiStatus s_battery_ui_status{};
static uint32_t s_battery_shutdown_last_check_ms = 0;
static uint8_t s_battery_shutdown_confirm_count = 0;
static BatteryShutdownReason s_battery_shutdown_last_reason = BatteryShutdownReason::None;
static uint32_t s_battery_ui_last_sample_ms = 0;
static uint32_t s_charger_ui_last_sample_ms = 0;
static uint32_t s_battery_runtime_last_sample_ms = 0;
static uint8_t s_battery_ui_sample_count = 0;

static bool s_battery_runtime_filter_ready = false;
static uint16_t s_battery_runtime_filtered_ma = 0;
static uint32_t s_battery_runtime_stable_since_ms = 0;
static portMUX_TYPE s_battery_runtime_event_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_battery_runtime_load_change_pending = false;

static bool take_battery_runtime_load_change()
{
    portENTER_CRITICAL(&s_battery_runtime_event_mux);
    const bool pending = s_battery_runtime_load_change_pending;
    s_battery_runtime_load_change_pending = false;
    portEXIT_CRITICAL(&s_battery_runtime_event_mux);
    return pending;
}

static void reset_battery_runtime_estimate(BatteryRuntimeEstimateState state,
                                           uint32_t now)
{
    s_battery_runtime_filter_ready = false;
    s_battery_runtime_filtered_ma = 0;
    s_battery_runtime_stable_since_ms = now;

    s_battery_ui_status.runtime_estimate_state = state;
    s_battery_ui_status.estimated_discharge_current_ma = 0;
    s_battery_ui_status.estimated_runtime_minutes = 0;
    s_battery_ui_status.estimate_stable_ms = 0;
    s_battery_ui_status.estimate_updated_ms = now;
}

static void update_battery_runtime_estimate(int16_t average_current_ma,
                                            uint16_t remaining_capacity_mah,
                                            uint32_t now)
{
    if (!s_battery_ui_status.valid || remaining_capacity_mah == 0) {
        reset_battery_runtime_estimate(BatteryRuntimeEstimateState::Unavailable, now);
        return;
    }

    if (s_battery_ui_status.charging) {
        reset_battery_runtime_estimate(BatteryRuntimeEstimateState::Charging, now);
        return;
    }

    if (s_battery_ui_status.external_power_good) {
        reset_battery_runtime_estimate(BatteryRuntimeEstimateState::ExternalPower, now);
        return;
    }

    // BQ27441 放电电流为负数；接近零时续航除法会被放大，不输出不可靠结果。
    if (average_current_ma >= -static_cast<int16_t>(BATTERY_RUNTIME_MIN_DISCHARGE_MA)) {
        reset_battery_runtime_estimate(BatteryRuntimeEstimateState::LowCurrent, now);
        return;
    }

    const uint16_t discharge_ma = static_cast<uint16_t>(-static_cast<int32_t>(average_current_ma));

    if (!s_battery_runtime_filter_ready) {
        s_battery_runtime_filter_ready = true;
        s_battery_runtime_filtered_ma = discharge_ma;
        s_battery_runtime_stable_since_ms = now;
    } else {
        const uint16_t previous = s_battery_runtime_filtered_ma;
        const uint16_t difference = discharge_ma > previous
            ? static_cast<uint16_t>(discharge_ma - previous)
            : static_cast<uint16_t>(previous - discharge_ma);
        const uint16_t relative_threshold = static_cast<uint16_t>(
            (static_cast<uint32_t>(previous) * BATTERY_RUNTIME_SHIFT_PERCENT) / 100UL);
        const uint16_t shift_threshold = relative_threshold > BATTERY_RUNTIME_SHIFT_MIN_MA
            ? relative_threshold
            : BATTERY_RUNTIME_SHIFT_MIN_MA;

        if (difference >= shift_threshold) {
            // 亮屏、切换功放/蓝牙、开始网络播放等负载突变后重新稳定。
            s_battery_runtime_filtered_ma = discharge_ma;
            s_battery_runtime_stable_since_ms = now;
        } else {
            // 约 60 秒时间常数的整数 EMA；10 秒采样时新值权重为 1/6。
            s_battery_runtime_filtered_ma = static_cast<uint16_t>(
                (static_cast<uint32_t>(previous) * 5UL + discharge_ma + 3UL) / 6UL);
        }
    }

    const uint32_t stable_ms = now - s_battery_runtime_stable_since_ms;
    s_battery_ui_status.estimated_discharge_current_ma = s_battery_runtime_filtered_ma;
    s_battery_ui_status.estimate_stable_ms = stable_ms;
    s_battery_ui_status.estimate_updated_ms = now;

    if (stable_ms < BATTERY_RUNTIME_SETTLE_MS || s_battery_runtime_filtered_ma == 0) {
        s_battery_ui_status.runtime_estimate_state =
            BatteryRuntimeEstimateState::Stabilizing;
        s_battery_ui_status.estimated_runtime_minutes = 0;
        return;
    }

    const uint32_t minutes =
        (static_cast<uint32_t>(remaining_capacity_mah) * 60UL +
         s_battery_runtime_filtered_ma / 2U) /
        s_battery_runtime_filtered_ma;

    s_battery_ui_status.runtime_estimate_state = BatteryRuntimeEstimateState::Ready;
    s_battery_ui_status.estimated_runtime_minutes = minutes;
}

static void apply_battery_runtime_load_change(uint32_t now)
{
    if (s_battery_ui_status.charging) {
        reset_battery_runtime_estimate(BatteryRuntimeEstimateState::Charging, now);
    } else if (s_battery_ui_status.external_power_good) {
        reset_battery_runtime_estimate(BatteryRuntimeEstimateState::ExternalPower, now);
    } else if (s_battery_ui_status.valid) {
        reset_battery_runtime_estimate(BatteryRuntimeEstimateState::Stabilizing, now);
        // 负载切换后下一轮立即重新采样，而不是继续使用切换前的电流。
        s_battery_runtime_last_sample_ms = 0;
    } else {
        reset_battery_runtime_estimate(BatteryRuntimeEstimateState::Unavailable, now);
    }
}

static void configure_bq27441_gpout_input()
{
    // BQ27441 GPOUT 通常为开漏/中断输出。
    // 原 GPIO1 不再接分压点，因此必须禁止 ADC 采样，只作为数字输入。
    pinMode(PIN_BQ27441_GPOUT, INPUT_PULLUP);

#if defined(ARDUINO_ARCH_ESP32)
    gpio_num_t gpio = static_cast<gpio_num_t>(PIN_BQ27441_GPOUT);
    gpio_set_direction(gpio, GPIO_MODE_INPUT);
    gpio_pullup_en(gpio);
    gpio_pulldown_dis(gpio);
#endif
}

static bool read_bq27441_gpout_level()
{
    return digitalRead(PIN_BQ27441_GPOUT) == HIGH;
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

// EWM104-BT62SP 模式脚经 2N7002 下拉：
// GPIO 为 HIGH 时 2N7002 导通，模块 BT_MODE 被拉低 = 发射模式；
// GPIO 为 LOW 时 2N7002 关闭，模块 BT_MODE 浮空 = 接收模式。
static constexpr bool BT_MODE_TX_GATE_LEVEL = true;

// BT_LINK 先按高电平表示已连接验证。
// 如果实测相反，只改这里。
static constexpr bool BT_LINK_ACTIVE_LEVEL = true;

// SW 更像“按键脚”，常见是低有效按下。
static constexpr bool BT_SW_ACTIVE_LEVEL = false;

bool s_ready = false;
bool s_bt_power_enabled = false;
// true 表示蓝牙模块被切到发射模式；false 表示接收/浮空模式。
bool s_bt_mode_transmit = false;
bool s_bt_switch_level = !BT_SW_ACTIVE_LEVEL;
bool s_amp_mute_enabled = true;
bool s_amp_shutdown_enabled = true;
bool s_backlight_enabled = true;

// TC118S / 电磁铁脉冲驱动：
// SOL_CTRL_A = MCP23017 GPB0，SOL_CTRL_B = MCP23017 GPB1。
// 默认停止态为 A=0/B=0，禁止长期通电。
static constexpr uint32_t SOLENOID_PULSE_MIN_MS = 20;
static constexpr uint32_t SOLENOID_PULSE_MAX_MS = 300;

bool s_solenoid_busy = false;
uint32_t s_solenoid_stop_at_ms = 0;
SolenoidDirection s_solenoid_last_direction = SolenoidDirection::B;

bool level_from_enabled(bool enabled, bool active_level)
{
    return enabled ? active_level : !active_level;
}

uint32_t clamp_solenoid_pulse_ms(uint32_t pulse_ms)
{
    if (pulse_ms < SOLENOID_PULSE_MIN_MS) return SOLENOID_PULSE_MIN_MS;
    if (pulse_ms > SOLENOID_PULSE_MAX_MS) return SOLENOID_PULSE_MAX_MS;
    return pulse_ms;
}

bool write_solenoid_levels(bool a_level, bool b_level)
{
    if (!mcp23017_u3_is_ready()) {
        return false;
    }

    // TC118S 是单路全桥控制，A/B 不能同时拉高；
    // 电磁铁只做一次动作，停止态统一 A=0/B=0。
    if (a_level && b_level) {
        LOGW("[SOL] 拒绝 A/B 同时有效");
        return false;
    }

    bool ok = true;
    ok &= mcp23017_u3_set_b(board::MCP_B_SOL_CTRL_A, a_level);
    ok &= mcp23017_u3_set_b(board::MCP_B_SOL_CTRL_B, b_level);
    return ok;
}

bool start_solenoid_pulse(SolenoidDirection direction, uint32_t pulse_ms)
{
    if (!mcp23017_u3_is_ready()) {
        return false;
    }

    const uint32_t safe_pulse_ms = clamp_solenoid_pulse_ms(pulse_ms);

    // 先回到停止态，再给目标方向脉冲，避免方向切换瞬间交叉导通。
    if (!write_solenoid_levels(false, false)) {
        return false;
    }

    const bool a_level = direction == SolenoidDirection::A;
    const bool b_level = direction == SolenoidDirection::B;

    if (!write_solenoid_levels(a_level, b_level)) {
        (void)write_solenoid_levels(false, false);
        return false;
    }

    s_solenoid_busy = true;
    s_solenoid_last_direction = direction;
    s_solenoid_stop_at_ms = millis() + safe_pulse_ms;

    LOGI("[SOL] pulse %s %lums", direction == SolenoidDirection::A ? "A" : "B", (unsigned long)safe_pulse_ms);
    return true;
}

}  // namespace

bool board_hw_control_begin()
{
    configure_bq27441_gpout_input();
    (void)bq27441_begin();

    s_ready = true;

    // 安全默认：
    // 蓝牙关闭，BT_MODE_CTRL 保持接收/浮空模式；
    // 功放保持“静音 + 关断”，等真正播放前再打开，减少开机爆破音。
    pinMode(board::PIN_BT_MODE_CTRL, OUTPUT);
    board_hw_set_bt_mode(false);
    board_hw_set_bt_power(false);
    board_hw_set_bt_switch(false);
    board_hw_set_amp_mute(true);
    board_hw_set_amp_shutdown(true);
    board_hw_solenoid_begin();

    LOGI("[硬件控制] 初始化成功 BQ27441=%d GPOUT=GPIO%d 蓝牙电源=MCPB%d 蓝牙模式=GPIO%d 蓝牙连接=MCPA%d 功放静音=MCPA%d 功放关断=MCPA%d",
         bq27441_is_ready() ? 1 : 0,
         PIN_BQ27441_GPOUT,
         board::MCP_B_BT_PWR_EN,
         board::PIN_BT_MODE_CTRL,
         board::MCP_A_BT_LINK,
         board::MCP_A_MUTE_EN,
         board::MCP_A_SHDN_EN);

    return s_ready;
}


void board_hw_i2c_service()
{
    // 总线恢复只在主循环执行，避免在任意设备事务内部重建 Wire。
    (void)i2c_bus_service();
    mcp23017_u3_service();
}

BatterySample board_hw_read_battery()
{
    BatterySample s{};

    configure_bq27441_gpout_input();
    s.gpout_level = read_bq27441_gpout_level();

    Bq27441Sample bq{};
    if (!bq27441_read(&bq)) {
        return s;
    }

    s.valid = bq.valid;
    s.mv_battery = bq.voltage_mv;
    s.mv_adc = 0;
    s.soc_percent = bq.soc_percent;
    s.average_current_ma = bq.average_current_ma;
    s.remaining_capacity_mah = bq.remaining_capacity_mah;
    s.full_charge_capacity_mah = bq.full_charge_capacity_mah;
    s.design_capacity_mah = bq.design_capacity_mah;
    s.flags = bq.flags;
    s.raw = bq.flags;
    s.state_of_health_percent = bq.state_of_health_percent;

    return s;
}

ChargerStatus board_hw_read_charger_status()
{
    ChargerStatus s{};

    if (!mcp23017_u3_is_ready()) {
        return s;
    }

    // GPIOB 一次读取同时得到 PG/CHG，避免每秒为两个 bit 发起两次 I2C 事务。
    uint8_t gpio_b = 0xFF;
    if (!mcp23017_u3_read_port_b(&gpio_b)) {
        return s;
    }

    const bool pg_level = (gpio_b & (1u << board::MCP_B_PG)) != 0;
    const bool chg_level = (gpio_b & (1u << board::MCP_B_CHG_STAT)) != 0;

    s.valid = true;
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

static void board_hw_update_charger_status_cache()
{
    const ChargerStatus chg = board_hw_read_charger_status();

    if (!chg.valid) {
        return;
    }

    const bool power_state_changed =
        s_battery_ui_status.external_power_good != chg.external_power_good ||
        s_battery_ui_status.charging != chg.charging;

    // PG/CHG 是数字状态，更新它不需要重新访问 BQ27441。
    // 这样插 USB 后，闪电图标最多 1 秒内出现。
    s_battery_ui_status.external_power_good = chg.external_power_good;
    s_battery_ui_status.charging = chg.charging;
    s_charger_ui_last_sample_ms = millis();

    if (power_state_changed) {
        board_hw_battery_estimate_notify_load_change();
    }
}

static void board_hw_update_battery_status_cache()
{
    const BatterySample bat = board_hw_read_battery();
    const ChargerStatus chg = board_hw_read_charger_status();

    BatteryUiStatus out = s_battery_ui_status;

    if (bat.valid) {
        out.valid = true;
        out.mv_battery = bat.mv_battery;
        out.mv_adc = 0;
        out.raw = bat.raw;
        out.percent = bat.soc_percent;
        out.average_current_ma = bat.average_current_ma;
        out.remaining_capacity_mah = bat.remaining_capacity_mah;
        out.full_charge_capacity_mah = bat.full_charge_capacity_mah;
        out.design_capacity_mah = bat.design_capacity_mah;
        out.flags = bat.flags;
        out.state_of_health_percent = bat.state_of_health_percent;
    }

    out.gpout_level = bat.gpout_level;

    if (chg.valid) {
        out.external_power_good = chg.external_power_good;
        out.charging = chg.charging;
        s_charger_ui_last_sample_ms = millis();
    }

    out.updated_ms = millis();

    s_battery_ui_status = out;
    s_battery_ui_last_sample_ms = out.updated_ms;
    s_battery_runtime_last_sample_ms = out.updated_ms;

    if (bat.valid) {
        update_battery_runtime_estimate(
            bat.average_current_ma,
            bat.remaining_capacity_mah,
            out.updated_ms);
    }

    if (s_battery_ui_sample_count < 255) {
        ++s_battery_ui_sample_count;
    }
}

void board_hw_battery_status_tick()
{
    const uint32_t now = millis();

    // PG/CHG 状态单独快刷。
    // 插 USB / 拔 USB 后，UI 闪电图标不需要等 1 分钟。
    if (s_charger_ui_last_sample_ms == 0 ||
        now - s_charger_ui_last_sample_ms >= CHARGER_UI_SAMPLE_INTERVAL_MS) {
        board_hw_update_charger_status_cache();
    }

    // 负载变化可能来自 AudioTask 或主循环；这里只消费事件并修改估算状态，
    // 保持电池缓存由 UI 任务单写。
    if (take_battery_runtime_load_change()) {
        apply_battery_runtime_load_change(now);
    }

    const bool boot_sampling =
        s_battery_ui_sample_count < BATTERY_UI_BOOT_SAMPLE_COUNT;

    const uint32_t full_interval_ms = boot_sampling
        ? BATTERY_UI_BOOT_SAMPLE_INTERVAL_MS
        : BATTERY_UI_STABLE_SAMPLE_INTERVAL_MS;

    const bool full_sample_due =
        s_battery_ui_last_sample_ms == 0 ||
        now - s_battery_ui_last_sample_ms >= full_interval_ms;

    if (full_sample_due) {
        // 第一次立即完整采样；上电前几次快速采样；稳定后每分钟一次。
        board_hw_update_battery_status_cache();
        return;
    }

    if (s_battery_runtime_last_sample_ms != 0 &&
        now - s_battery_runtime_last_sample_ms < BATTERY_RUNTIME_SAMPLE_INTERVAL_MS) {
        return;
    }

    // 续航估算只额外读取两个寄存器，不提高完整电池采样频率。
    Bq27441RuntimeSample runtime{};
    s_battery_runtime_last_sample_ms = now;
    if (!bq27441_read_runtime(&runtime) || !runtime.valid) {
        if (!bq27441_is_ready()) {
            reset_battery_runtime_estimate(
                BatteryRuntimeEstimateState::Unavailable,
                now);
        }
        return;
    }

    s_battery_ui_status.average_current_ma = runtime.average_current_ma;
    s_battery_ui_status.remaining_capacity_mah = runtime.remaining_capacity_mah;
    s_battery_ui_status.updated_ms = now;

    update_battery_runtime_estimate(
        runtime.average_current_ma,
        runtime.remaining_capacity_mah,
        now);
}

BatteryUiStatus board_hw_get_battery_status_cached()
{
    return s_battery_ui_status;
}

const char* board_hw_battery_runtime_state_label(BatteryRuntimeEstimateState state)
{
    switch (state) {
        case BatteryRuntimeEstimateState::Charging: return "充电中";
        case BatteryRuntimeEstimateState::ExternalPower: return "外接电源";
        case BatteryRuntimeEstimateState::LowCurrent: return "负载过低";
        case BatteryRuntimeEstimateState::Stabilizing: return "计算中";
        case BatteryRuntimeEstimateState::Ready: return "稳定";
        case BatteryRuntimeEstimateState::Unavailable:
        default: return "不可用";
    }
}

void board_hw_battery_estimate_notify_load_change()
{
    portENTER_CRITICAL(&s_battery_runtime_event_mux);
    s_battery_runtime_load_change_pending = true;
    portEXIT_CRITICAL(&s_battery_runtime_event_mux);
}

const char* board_hw_battery_shutdown_reason_label(BatteryShutdownReason reason)
{
    switch (reason) {
        case BatteryShutdownReason::SocFinal: return "SOCF极低电量";
        case BatteryShutdownReason::SocCritical: return "电量低于5%";
        case BatteryShutdownReason::VoltageCritical: return "电压低于3.40V";
        case BatteryShutdownReason::GpoutLow: return "GPOUT低电量告警";
        case BatteryShutdownReason::None:
        default: return "无";
    }
}

BatteryShutdownReason board_hw_battery_shutdown_reason()
{
    const uint32_t now = millis();
    if (s_battery_shutdown_last_check_ms != 0 &&
        now - s_battery_shutdown_last_check_ms < BATTERY_SHUTDOWN_CHECK_INTERVAL_MS) {
        return BatteryShutdownReason::None;
    }
    s_battery_shutdown_last_check_ms = now;

    configure_bq27441_gpout_input();
    s_battery_ui_status.gpout_level = read_bq27441_gpout_level();

    if (!s_battery_ui_status.gpout_level) {
        board_hw_update_battery_status_cache();
    } else {
        board_hw_battery_status_tick();
    }

    const BatteryUiStatus bat = s_battery_ui_status;

    BatteryShutdownReason reason = BatteryShutdownReason::None;

    if (bat.valid && !bat.external_power_good && !bat.charging) {
        const bool socf = (bat.flags & BQ27441_FLAG_SOCF) != 0;
        const bool soc1 = (bat.flags & BQ27441_FLAG_SOC1) != 0;
        const bool gpout_asserted = !bat.gpout_level;

        if (socf) {
            reason = BatteryShutdownReason::SocFinal;
        } else if (bat.percent <= BATTERY_SHUTDOWN_SOC_PERCENT) {
            reason = BatteryShutdownReason::SocCritical;
        } else if (bat.mv_battery > 0 && bat.mv_battery <= BATTERY_SHUTDOWN_VOLTAGE_MV) {
            reason = BatteryShutdownReason::VoltageCritical;
        } else if (gpout_asserted &&
                   (soc1 ||
                    bat.percent <= BATTERY_GPOUT_CONFIRM_SOC_PERCENT ||
                    (bat.mv_battery > 0 && bat.mv_battery <= BATTERY_GPOUT_CONFIRM_VOLTAGE_MV))) {
            reason = BatteryShutdownReason::GpoutLow;
        }
    }

    if (reason == BatteryShutdownReason::None) {
        s_battery_shutdown_confirm_count = 0;
        s_battery_shutdown_last_reason = BatteryShutdownReason::None;
        return BatteryShutdownReason::None;
    }

    if (reason != s_battery_shutdown_last_reason) {
        s_battery_shutdown_confirm_count = 0;
        s_battery_shutdown_last_reason = reason;
    }

    if (s_battery_shutdown_confirm_count < 255) {
        ++s_battery_shutdown_confirm_count;
    }

    if (s_battery_shutdown_confirm_count >= BATTERY_SHUTDOWN_CONFIRM_COUNT) {
        LOGW("[电池] 低电量关机确认：原因=%s SOC=%u%% 电压=%lumV flags=0x%04X GPOUT=%d",
             board_hw_battery_shutdown_reason_label(reason),
             (unsigned)bat.percent,
             (unsigned long)bat.mv_battery,
             (unsigned)bat.flags,
             bat.gpout_level ? 1 : 0);
        return reason;
    }

    LOGW("[电池] 低电量关机候选：原因=%s 确认=%u/%u SOC=%u%% 电压=%lumV flags=0x%04X GPOUT=%d",
         board_hw_battery_shutdown_reason_label(reason),
         (unsigned)s_battery_shutdown_confirm_count,
         (unsigned)BATTERY_SHUTDOWN_CONFIRM_COUNT,
         (unsigned)bat.percent,
         (unsigned long)bat.mv_battery,
         (unsigned)bat.flags,
         bat.gpout_level ? 1 : 0);
    return BatteryShutdownReason::None;
}

bool board_hw_set_bt_power(bool enabled)
{
    if (!mcp23017_u3_is_ready()) return false;

    const bool level = level_from_enabled(enabled, BT_PWR_ACTIVE_LEVEL);
    if (!mcp23017_u3_set_b(board::MCP_B_BT_PWR_EN, level)) {
        return false;
    }

    const bool changed = s_bt_power_enabled != enabled;
    s_bt_power_enabled = enabled;
    if (changed) board_hw_battery_estimate_notify_load_change();
    LOGI("[硬件控制] 蓝牙电源 %s 电平=%d", enabled ? "开启" : "关闭", level ? 1 : 0);
    return true;
}

bool board_hw_get_bt_power()
{
    return s_bt_power_enabled;
}

bool board_hw_set_bt_mode(bool transmit)
{
    const bool level = transmit ? BT_MODE_TX_GATE_LEVEL : !BT_MODE_TX_GATE_LEVEL;
    digitalWrite(board::PIN_BT_MODE_CTRL, level ? HIGH : LOW);

    const bool changed = s_bt_mode_transmit != transmit;
    s_bt_mode_transmit = transmit;
    if (changed) board_hw_battery_estimate_notify_load_change();
    LOGI("[硬件控制] 蓝牙模式 %s GPIO%d=%d 模块BT_MODE=%s",
         transmit ? "发射" : "接收",
         board::PIN_BT_MODE_CTRL,
         level ? 1 : 0,
         transmit ? "拉低" : "浮空");
    return true;
}

bool board_hw_get_bt_mode()
{
    return s_bt_mode_transmit;
}

bool board_hw_read_bt_link(bool* linked)
{
    if (!linked) return false;
    if (!mcp23017_u3_is_ready()) return false;

    uint8_t gpio_a = 0xFF;
    if (!mcp23017_u3_read_port_a(&gpio_a)) {
        return false;
    }

    const bool level = (gpio_a & (1u << board::MCP_A_BT_LINK)) != 0;
    *linked = (level == BT_LINK_ACTIVE_LEVEL);
    return true;
}

bool board_hw_is_bt_linked()
{
    bool linked = false;
    return board_hw_read_bt_link(&linked) && linked;
}

bool board_hw_set_bt_switch(bool pressed)
{
    if (!mcp23017_u3_is_ready()) return false;

    const bool level = level_from_enabled(pressed, BT_SW_ACTIVE_LEVEL);
    if (!mcp23017_u3_set_a(board::MCP_A_BT_SW_CTRL, level)) {
        return false;
    }

    s_bt_switch_level = level;
    LOGI("[硬件控制] 蓝牙按键 按下=%d 电平=%d", pressed ? 1 : 0, level ? 1 : 0);
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

    const bool changed = s_backlight_enabled != enabled;
    s_backlight_enabled = enabled;
    if (changed) board_hw_battery_estimate_notify_load_change();
    LOGI("[硬件控制] 背光 %s", enabled ? "开启" : "关闭");
    return true;
}

bool board_hw_get_backlight()
{
    return s_backlight_enabled;
}

bool board_hw_solenoid_begin()
{
    s_solenoid_busy = false;
    s_solenoid_stop_at_ms = 0;

    const bool ok = write_solenoid_levels(false, false);
    LOGI("[SOL] begin %s A=MCPB%d B=MCPB%d",
         ok ? "ok" : "fail",
         board::MCP_B_SOL_CTRL_A,
         board::MCP_B_SOL_CTRL_B);
    return ok;
}

bool board_hw_solenoid_stop()
{
    const bool ok = write_solenoid_levels(false, false);
    if (ok) {
        s_solenoid_busy = false;
        s_solenoid_stop_at_ms = 0;
    }
    return ok;
}

bool board_hw_solenoid_pulse_a(uint32_t pulse_ms)
{
    return start_solenoid_pulse(SolenoidDirection::A, pulse_ms);
}

bool board_hw_solenoid_pulse_b(uint32_t pulse_ms)
{
    return start_solenoid_pulse(SolenoidDirection::B, pulse_ms);
}

bool board_hw_solenoid_flip(uint32_t pulse_ms)
{
    const SolenoidDirection next =
        s_solenoid_last_direction == SolenoidDirection::A
            ? SolenoidDirection::B
            : SolenoidDirection::A;

    return start_solenoid_pulse(next, pulse_ms);
}

void board_hw_solenoid_tick()
{
    if (!s_solenoid_busy) {
        return;
    }

    if (static_cast<int32_t>(s_solenoid_stop_at_ms - millis()) > 0) {
        return;
    }

    if (board_hw_solenoid_stop()) {
        LOGD("[SOL] auto stop");
    }
}

bool board_hw_solenoid_is_busy()
{
    return s_solenoid_busy;
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

    if (s_amp_mute_enabled == enabled) {
        // 状态未变化时不重复写 MCP23017，避免切歌时出现多次相同静音日志和无意义 I2C 操作。
        return true;
    }

    const bool level = level_from_enabled(enabled, AMP_MUTE_ACTIVE_LEVEL);
    if (!mcp23017_u3_set_a(board::MCP_A_MUTE_EN, level)) {
        return false;
    }

    s_amp_mute_enabled = enabled;
    board_hw_battery_estimate_notify_load_change();
    LOGD("[硬件控制] 功放静音 %s 电平=%d", enabled ? "开启" : "关闭", level ? 1 : 0);
    return true;
}

bool board_hw_get_amp_mute()
{
    return s_amp_mute_enabled;
}

bool board_hw_set_amp_shutdown(bool enabled)
{
    if (!mcp23017_u3_is_ready()) return false;

    const bool changed = s_amp_shutdown_enabled != enabled;
    const bool level = level_from_enabled(enabled, AMP_SHDN_ACTIVE_LEVEL);
    if (!mcp23017_u3_set_a(board::MCP_A_SHDN_EN, level)) {
        return false;
    }

    s_amp_shutdown_enabled = enabled;
    if (changed) board_hw_battery_estimate_notify_load_change();
    LOGI("[硬件控制] 功放关断 %s 电平=%d", enabled ? "开启" : "关闭", level ? 1 : 0);
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

    bool bt_linked = false;
    (void)board_hw_read_bt_link(&bt_linked);

    LOGD("[硬件控制] 状态 bq_valid=%d 电池=%lumV soc=%u%% current=%dmA cap=%u/%umAh flags=0x%04X gpout=%d 蓝牙电源=%d 蓝牙发射=%d 蓝牙连接=%d 静音=%d 关断=%d PG=%d CHG=%d",
         bat.valid ? 1 : 0,
         (unsigned long)bat.mv_battery,
         static_cast<unsigned>(bat.soc_percent),
         static_cast<int>(bat.average_current_ma),
         static_cast<unsigned>(bat.remaining_capacity_mah),
         static_cast<unsigned>(bat.full_charge_capacity_mah),
         static_cast<unsigned>(bat.flags),
         bat.gpout_level ? 1 : 0,
         s_bt_power_enabled ? 1 : 0,
         s_bt_mode_transmit ? 1 : 0,
         bt_linked ? 1 : 0,
         s_amp_mute_enabled ? 1 : 0,
         s_amp_shutdown_enabled ? 1 : 0,
         pg_level ? 1 : 0,
         chg_level ? 1 : 0);
}

void board_hw_power_off()
{
    LOGI("[硬件控制] 关机：释放 POWER_CTRL GPIO%d", PIN_POWER_CTRL);

    pinMode(PIN_POWER_CTRL, OUTPUT);
    digitalWrite(PIN_POWER_CTRL, LOW);
}
