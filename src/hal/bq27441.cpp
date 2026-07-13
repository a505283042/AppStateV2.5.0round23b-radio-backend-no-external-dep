#include "hal/bq27441.h"

#include <Wire.h>
#include <Preferences.h>
#include <stdio.h>
#include <string.h>

#include "hal/i2c_bus_lock.h"
#include "utils/log.h"

namespace {

constexpr uint8_t BQ27441_ADDR = 0x55;

constexpr uint8_t CMD_CONTROL = 0x00;
constexpr uint8_t CMD_VOLTAGE = 0x04;
constexpr uint8_t CMD_FLAGS = 0x06;
constexpr uint8_t CMD_REMAINING_CAPACITY = 0x0C;
constexpr uint8_t CMD_FULL_CHARGE_CAPACITY = 0x0E;
constexpr uint8_t CMD_AVERAGE_CURRENT = 0x10;
constexpr uint8_t CMD_STATE_OF_CHARGE = 0x1C;
constexpr uint8_t CMD_STATE_OF_HEALTH = 0x20;
constexpr uint8_t CMD_EXT_OPCONFIG = 0x3A;
constexpr uint8_t CMD_EXT_DESIGN_CAPACITY = 0x3C;

constexpr uint8_t EXT_DATA_CLASS = 0x3E;
constexpr uint8_t EXT_DATA_BLOCK = 0x3F;
constexpr uint8_t EXT_BLOCK_DATA = 0x40;
constexpr uint8_t EXT_BLOCK_CHECKSUM = 0x60;
constexpr uint8_t EXT_BLOCK_CONTROL = 0x61;

constexpr uint8_t CLASS_DISCHARGE = 49;
constexpr uint8_t CLASS_REGISTERS = 64;
constexpr uint8_t CLASS_STATE = 82;

constexpr uint8_t STATE_OFF_DESIGN_CAPACITY = 10;
constexpr uint8_t STATE_OFF_DESIGN_ENERGY = 12;
constexpr uint8_t STATE_OFF_TERMINATE_VOLTAGE = 16;
constexpr uint8_t STATE_OFF_TAPER_RATE = 27;

constexpr uint8_t DISCHARGE_OFF_SOC1_SET = 0;
constexpr uint8_t DISCHARGE_OFF_SOC1_CLEAR = 1;
constexpr uint8_t DISCHARGE_OFF_SOCF_SET = 2;
constexpr uint8_t DISCHARGE_OFF_SOCF_CLEAR = 3;

constexpr uint8_t REGISTERS_OFF_OPCONFIG = 0;

constexpr uint16_t CTRL_CONTROL_STATUS = 0x0000;
constexpr uint16_t CTRL_DEVICE_TYPE = 0x0001;
constexpr uint16_t CTRL_SET_CFGUPDATE = 0x0013;
constexpr uint16_t CTRL_SEALED = 0x0020;
constexpr uint16_t CTRL_EXIT_CFGUPDATE = 0x0043;
constexpr uint16_t CTRL_EXIT_RESIM = 0x0044;
constexpr uint16_t CTRL_UNSEAL_KEY = 0x8000;

constexpr uint16_t STATUS_INITCOMP = (1u << 7);
constexpr uint16_t STATUS_SLEEP = (1u << 4);
constexpr uint16_t STATUS_HIBERNATE = (1u << 6);
constexpr uint16_t STATUS_SS = (1u << 13);
constexpr uint16_t STATUS_WDRESET = (1u << 15);
constexpr uint16_t OPCONFIG_GPIOPOL = (1u << 11);
constexpr uint16_t OPCONFIG_BATLOWEN = (1u << 2);

constexpr uint32_t BQ27441_WARN_INTERVAL_MS = 60UL * 1000UL;
constexpr uint32_t BQ27441_RETRY_AFTER_RUNTIME_FAILURE_MS = 15UL * 1000UL;
constexpr uint32_t BQ27441_RETRY_AFTER_MISSING_MS = 60UL * 1000UL;
constexpr uint32_t BQ27441_CONFIG_RETRY_MS = 15UL * 1000UL;

#ifndef BQ27441_DESIGN_CAPACITY_MAH
#define BQ27441_DESIGN_CAPACITY_MAH 500
#endif

#ifndef BQ27441_DESIGN_ENERGY_MWH
#define BQ27441_DESIGN_ENERGY_MWH 1850
#endif

constexpr uint16_t BQ27441_CAPACITY_MIN_MAH = 100;
constexpr uint16_t BQ27441_CAPACITY_MAX_MAH = 5000;
constexpr uint16_t BQ27441_NOMINAL_VOLTAGE_MV = 3700;
constexpr char BQ27441_PREFS_NAMESPACE[] = "battery";
constexpr char BQ27441_PREFS_CAPACITY_KEY[] = "design_mah";

#ifndef BQ27441_TERMINATE_VOLTAGE_MV
#define BQ27441_TERMINATE_VOLTAGE_MV 3300
#endif

#ifndef BQ27441_TAPER_RATE
#define BQ27441_TAPER_RATE 100
#endif

#ifndef BQ27441_SOC1_SET_PERCENT
#define BQ27441_SOC1_SET_PERCENT 10
#endif

#ifndef BQ27441_SOC1_CLEAR_PERCENT
#define BQ27441_SOC1_CLEAR_PERCENT 15
#endif

#ifndef BQ27441_SOCF_SET_PERCENT
#define BQ27441_SOCF_SET_PERCENT 5
#endif

#ifndef BQ27441_SOCF_CLEAR_PERCENT
#define BQ27441_SOCF_CLEAR_PERCENT 8
#endif

#ifndef BQ27441_READ_SOH
#define BQ27441_READ_SOH 0
#endif

bool s_ready = false;
uint8_t s_last_i2c_error = 0;
uint32_t s_last_warn_ms = 0;
uint32_t s_last_read_warn_ms = 0;
uint8_t s_consecutive_mandatory_read_failures = 0;
uint8_t s_last_failed_cmd = 0;
bool s_missing_warned_once = false;
uint16_t s_design_capacity_mah = 0;
uint16_t s_target_design_capacity_mah = BQ27441_DESIGN_CAPACITY_MAH;
bool s_capacity_pref_loaded = false;
bool s_config_attempted_this_boot = false;
bool s_config_ok_this_boot = false;
bool s_ever_ready = false;
bool s_scan_done_this_boot = false;
uint32_t s_next_begin_attempt_ms = 0;
uint32_t s_seen_bus_generation = 0;
uint8_t s_consecutive_runtime_read_failures = 0;
uint32_t s_last_runtime_warn_ms = 0;
uint32_t s_next_config_attempt_ms = 0;

enum class BqConfigTrigger : uint8_t {
    Startup = 0,
    Runtime,
    User,
};

struct BqDiagnosticSnapshot {
    uint16_t control_status = 0;
    uint16_t flags = 0;
    uint16_t voltage_mv = 0;
    int16_t average_current_ma = 0;
    uint16_t soc_percent = 0;
    uint16_t remaining_capacity_mah = 0;
    uint16_t full_charge_capacity_mah = 0;
    uint16_t design_capacity_mah = 0;
    uint16_t valid_mask = 0;
};

enum BqDiagnosticField : uint16_t {
    DIAG_CONTROL = 1u << 0,
    DIAG_FLAGS = 1u << 1,
    DIAG_VOLTAGE = 1u << 2,
    DIAG_CURRENT = 1u << 3,
    DIAG_SOC = 1u << 4,
    DIAG_RM = 1u << 5,
    DIAG_FCC = 1u << 6,
    DIAG_DESIGN = 1u << 7,
};

const char* config_trigger_name(BqConfigTrigger trigger)
{
    switch (trigger) {
        case BqConfigTrigger::Startup: return "开机";
        case BqConfigTrigger::Runtime: return "运行时";
        case BqConfigTrigger::User: return "用户设置";
        default: return "未知";
    }
}

uint16_t clamp_design_capacity_mah(uint16_t capacity_mah)
{
    if (capacity_mah < BQ27441_CAPACITY_MIN_MAH) return BQ27441_CAPACITY_MIN_MAH;
    if (capacity_mah > BQ27441_CAPACITY_MAX_MAH) return BQ27441_CAPACITY_MAX_MAH;
    return capacity_mah;
}

void load_design_capacity_pref_once()
{
    if (s_capacity_pref_loaded) return;
    s_capacity_pref_loaded = true;

    Preferences pref;
    uint16_t value = BQ27441_DESIGN_CAPACITY_MAH;
    // 以读写模式打开：首次使用时自动创建命名空间，避免只读打开输出 NOT_FOUND。
    // 这里只读取配置；除首次创建命名空间外不会反复写入容量值。
    if (pref.begin(BQ27441_PREFS_NAMESPACE, false)) {
        value = pref.getUShort(BQ27441_PREFS_CAPACITY_KEY, BQ27441_DESIGN_CAPACITY_MAH);
        pref.end();
    }

    s_target_design_capacity_mah = clamp_design_capacity_mah(value);
    LOGI("[BQ27441] 目标设计容量=%umAh", (unsigned)s_target_design_capacity_mah);
}

uint16_t design_energy_mwh_for_capacity(uint16_t capacity_mah)
{
    const uint32_t energy =
        ((uint32_t)capacity_mah * BQ27441_NOMINAL_VOLTAGE_MV + 500UL) / 1000UL;
    return static_cast<uint16_t>(energy > 0xFFFFUL ? 0xFFFFUL : energy);
}

uint16_t taper_rate_for_capacity(uint16_t capacity_mah)
{
    // 延续原 500mAh -> TaperRate 100 的配置比例。
    const uint32_t rate = ((uint32_t)capacity_mah * BQ27441_TAPER_RATE + 250UL) / 500UL;
    if (rate == 0) return 1;
    return static_cast<uint16_t>(rate > 0xFFFFUL ? 0xFFFFUL : rate);
}

const char* known_i2c_label(uint8_t addr)
{
    switch (addr) {
        case 0x20: return "MCP23017";
        case 0x51: return "PCF85063/AT24C";
        case 0x55: return "BQ27441";
        default: return "";
    }
}

bool warn_due(uint32_t& last_ms, uint32_t interval_ms)
{
    const uint32_t now = millis();
    if (now - last_ms < interval_ms) return false;
    last_ms = now;
    return true;
}

bool i2c_ready_for_bq()
{
    if (i2c_bus_io_allowed()) return true;
    s_last_i2c_error = 0xFE;
    return false;
}

void log_i2c_scan_once()
{
    // 全地址扫描只允许在“本次开机从未成功识别电量计”时执行一次。
    // 运行中总线异常时扫描 126 个地址只会进一步占用总线。
    if (s_scan_done_this_boot || s_ever_ready || !i2c_ready_for_bq()) return;
    s_scan_done_this_boot = true;

    // 只探测本机已知地址，避免总线异常时扫描 126 个地址造成数秒阻塞。
    static constexpr uint8_t known_addrs[] = {0x20, 0x51, 0x55};

    char buf[160];
    size_t pos = 0;
    int found = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "[BQ27441] I2C已知地址探测：");

    i2c_bus_lock();
    for (uint8_t addr : known_addrs) {
        Wire.beginTransmission(addr);
        const uint8_t err = Wire.endTransmission(true);
        if (err == 0) {
            ++found;
            const char* label = known_i2c_label(addr);
            pos += snprintf(buf + pos, sizeof(buf) - pos, " 0x%02X(%s)", addr, label);
        }
    }
    i2c_bus_unlock();

    if (found == 0 && pos + 5 < sizeof(buf)) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, " 无");
    }

    LOGI("%s", buf);
}

bool probe_addr(uint8_t addr)
{
    if (!i2c_ready_for_bq()) return false;
    i2c_bus_lock();
    Wire.beginTransmission(addr);
    const uint8_t err = Wire.endTransmission(true);
    i2c_bus_unlock();
    s_last_i2c_error = err;
    return err == 0;
}

bool write_bytes_locked(uint8_t addr, uint8_t reg, const uint8_t* data, size_t len)
{
    Wire.beginTransmission(addr);
    Wire.write(reg);
    for (size_t i = 0; i < len; ++i) {
        Wire.write(data[i]);
    }
    const uint8_t err = Wire.endTransmission(true);
    s_last_i2c_error = err;
    if (err != 0) {
        s_last_failed_cmd = reg;
        return false;
    }
    return true;
}

bool write_byte_locked(uint8_t addr, uint8_t reg, uint8_t value)
{
    return write_bytes_locked(addr, reg, &value, 1);
}

bool read_bytes_locked(uint8_t addr, uint8_t reg, uint8_t* out, size_t len)
{
    if (!out || len == 0) return false;

    Wire.beginTransmission(addr);
    Wire.write(reg);
    uint8_t err = Wire.endTransmission(true);
    s_last_i2c_error = err;
    if (err != 0) {
        s_last_failed_cmd = reg;
        return false;
    }

    delayMicroseconds(300);

    // 明确使用 uint8_t + size_t + bool 重载，避免 Arduino-ESP32 2.x 下重载解析歧义。
    const size_t received = Wire.requestFrom(addr, len, true);
    if (received != len || Wire.available() < static_cast<int>(len)) {
        s_last_i2c_error = static_cast<uint8_t>(0xF0u | (received & 0x0Fu));
        s_last_failed_cmd = reg;
        return false;
    }

    for (size_t i = 0; i < len; ++i) {
        out[i] = Wire.read();
    }
    s_last_i2c_error = 0;
    return true;
}

bool read_word_at_locked(uint8_t addr, uint8_t cmd, uint16_t* out)
{
    uint8_t data[2] = {0, 0};
    if (!out || !read_bytes_locked(addr, cmd, data, sizeof(data))) return false;
    *out = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
    return true;
}

bool read_word_at(uint8_t addr, uint8_t cmd, uint16_t* out)
{
    if (!i2c_ready_for_bq()) return false;
    i2c_bus_lock();
    const bool ok = read_word_at_locked(addr, cmd, out);
    i2c_bus_unlock();
    return ok;
}

bool read_word(uint8_t cmd, uint16_t* out)
{
    return read_word_at(BQ27441_ADDR, cmd, out);
}

bool execute_control_locked(uint16_t subcmd)
{
    const uint8_t data[2] = {
        static_cast<uint8_t>(subcmd & 0xFF),
        static_cast<uint8_t>(subcmd >> 8),
    };
    return write_bytes_locked(BQ27441_ADDR, CMD_CONTROL, data, sizeof(data));
}

bool execute_control(uint16_t subcmd)
{
    if (!i2c_ready_for_bq()) return false;
    i2c_bus_lock();
    const bool ok = execute_control_locked(subcmd);
    i2c_bus_unlock();
    return ok;
}

bool control_word_at(uint8_t addr, uint16_t subcmd, uint16_t* out)
{
    if (!out) return false;
    if (!i2c_ready_for_bq()) return false;

    i2c_bus_lock();
    const bool wrote = [&]() -> bool {
        const uint8_t data[2] = {
            static_cast<uint8_t>(subcmd & 0xFF),
            static_cast<uint8_t>(subcmd >> 8),
        };
        return write_bytes_locked(addr, CMD_CONTROL, data, sizeof(data));
    }();
    if (!wrote) {
        i2c_bus_unlock();
        return false;
    }

    delay(2);
    const bool ok = read_word_at_locked(addr, CMD_CONTROL, out);
    i2c_bus_unlock();
    return ok;
}

bool control_word(uint16_t subcmd, uint16_t* out)
{
    return control_word_at(BQ27441_ADDR, subcmd, out);
}

bool detect_bq27441(uint16_t* out_device_type,
                     uint16_t* out_control_status,
                     uint16_t* out_voltage_mv,
                     uint16_t* out_flags)
{
    *out_device_type = 0;
    *out_control_status = 0;
    *out_voltage_mv = 0;
    *out_flags = 0;

    bool ack = false;
    for (uint8_t attempt = 0; attempt < 3; ++attempt) {
        ack = probe_addr(BQ27441_ADDR);
        if (ack) break;
        delay(50);
    }

    if (!ack) {
        if (!s_missing_warned_once || warn_due(s_last_warn_ms, BQ27441_WARN_INTERVAL_MS)) {
            LOGW("[BQ27441] 初始化失败：I2C地址0x%02X无ACK err=%u，请检查SDA/SCL/电源/上拉；GPOUT接IO1不能代替I2C通信",
                 BQ27441_ADDR,
                 s_last_i2c_error);
            s_missing_warned_once = true;
            log_i2c_scan_once();
        }
        return false;
    }

    const bool got_device = control_word(CTRL_DEVICE_TYPE, out_device_type);
    const bool got_status = control_word(CTRL_CONTROL_STATUS, out_control_status);
    const bool got_voltage = read_word(CMD_VOLTAGE, out_voltage_mv);
    const bool got_flags = read_word(CMD_FLAGS, out_flags);

    if (!got_voltage || *out_voltage_mv == 0 || *out_voltage_mv > 6000) {
        if (warn_due(s_last_warn_ms, BQ27441_WARN_INTERVAL_MS)) {
            LOGW("[BQ27441] 初始化失败：地址0x%02X有ACK，但标准命令读取异常 voltage_ok=%d voltage=%umV err=%u",
                 BQ27441_ADDR,
                 got_voltage ? 1 : 0,
                 *out_voltage_mv,
                 s_last_i2c_error);
            log_i2c_scan_once();
        }
        return false;
    }

    return true;
}

bool is_voltage_sane(uint16_t voltage_mv)
{
    return voltage_mv >= 2000 && voltage_mv <= 6000;
}

bool flags_has(uint16_t flags, uint16_t mask)
{
    return (flags & mask) != 0;
}

bool wait_for_cfgupdate(bool enabled, uint32_t timeout_ms)
{
    const uint32_t start = millis();
    while (millis() - start < timeout_ms) {
        uint16_t flags = 0;
        if (read_word(CMD_FLAGS, &flags)) {
            const bool in_cfg = flags_has(flags, BQ27441_FLAG_CFGUPMODE);
            if (in_cfg == enabled) return true;
        }
        delay(10);
    }
    return false;
}

uint8_t checksum_block(const uint8_t block[32])
{
    uint16_t sum = 0;
    for (uint8_t i = 0; i < 32; ++i) {
        sum += block[i];
    }
    return 0xFF - (uint8_t)(sum & 0xFF);
}

bool write_extended_data_locked(uint8_t class_id, uint8_t offset, const uint8_t* data, uint8_t len)
{
    if (!data || len == 0 || len > 32) return false;
    if ((offset % 32) + len > 32) return false;

    uint8_t block[32] = {0};
    const uint8_t block_index = offset / 32;
    const uint8_t block_offset = offset % 32;

    if (!write_byte_locked(BQ27441_ADDR, EXT_BLOCK_CONTROL, 0x00)) return false;
    delay(1);
    if (!write_byte_locked(BQ27441_ADDR, EXT_DATA_CLASS, class_id)) return false;
    delay(1);
    if (!write_byte_locked(BQ27441_ADDR, EXT_DATA_BLOCK, block_index)) return false;
    delay(2);
    if (!read_bytes_locked(BQ27441_ADDR, EXT_BLOCK_DATA, block, sizeof(block))) return false;

    memcpy(block + block_offset, data, len);

    for (uint8_t i = 0; i < len; ++i) {
        if (!write_byte_locked(BQ27441_ADDR, EXT_BLOCK_DATA + block_offset + i, data[i])) {
            return false;
        }
        delay(1);
    }

    const uint8_t csum = checksum_block(block);
    if (!write_byte_locked(BQ27441_ADDR, EXT_BLOCK_CHECKSUM, csum)) return false;
    delay(2);
    return true;
}

bool write_extended_u16_be_locked(uint8_t class_id, uint8_t offset, uint16_t value)
{
    const uint8_t data[2] = {
        static_cast<uint8_t>(value >> 8),
        static_cast<uint8_t>(value & 0xFF),
    };
    return write_extended_data_locked(class_id, offset, data, sizeof(data));
}

bool write_extended_u8_locked(uint8_t class_id, uint8_t offset, uint8_t value)
{
    return write_extended_data_locked(class_id, offset, &value, 1);
}

bool capture_diagnostic_snapshot(BqDiagnosticSnapshot* out)
{
    if (!out) return false;
    *out = BqDiagnosticSnapshot{};

    uint16_t current_raw = 0;
    if (control_word(CTRL_CONTROL_STATUS, &out->control_status)) out->valid_mask |= DIAG_CONTROL;
    if (read_word(CMD_FLAGS, &out->flags)) out->valid_mask |= DIAG_FLAGS;
    if (read_word(CMD_VOLTAGE, &out->voltage_mv)) out->valid_mask |= DIAG_VOLTAGE;
    if (read_word(CMD_AVERAGE_CURRENT, &current_raw)) {
        out->average_current_ma = static_cast<int16_t>(current_raw);
        out->valid_mask |= DIAG_CURRENT;
    }
    if (read_word(CMD_STATE_OF_CHARGE, &out->soc_percent)) out->valid_mask |= DIAG_SOC;
    if (read_word(CMD_REMAINING_CAPACITY, &out->remaining_capacity_mah)) out->valid_mask |= DIAG_RM;
    if (read_word(CMD_FULL_CHARGE_CAPACITY, &out->full_charge_capacity_mah)) out->valid_mask |= DIAG_FCC;
    if (read_word(CMD_EXT_DESIGN_CAPACITY, &out->design_capacity_mah)) out->valid_mask |= DIAG_DESIGN;

    return (out->valid_mask & (DIAG_CONTROL | DIAG_FLAGS | DIAG_VOLTAGE)) ==
           (DIAG_CONTROL | DIAG_FLAGS | DIAG_VOLTAGE);
}

void log_diagnostic_snapshot(const char* stage,
                             BqConfigTrigger trigger,
                             const BqDiagnosticSnapshot& s)
{
    LOGI("[BQ27441][重启诊断][%s][%s] valid=0x%02X control=0x%04X INITCOMP=%d WDRESET=%d SEALED=%d SLEEP=%d HIBERNATE=%d flags=0x%04X[%s] ITPOR=%d CFGUP=%d BAT=%d voltage=%umV current=%dmA SOC=%u%% RM=%umAh FCC=%umAh Design=%umAh target=%umAh",
         config_trigger_name(trigger),
         stage ? stage : "未知",
         (unsigned)s.valid_mask,
         s.control_status,
         (s.control_status & STATUS_INITCOMP) ? 1 : 0,
         (s.control_status & STATUS_WDRESET) ? 1 : 0,
         (s.control_status & STATUS_SS) ? 1 : 0,
         (s.control_status & STATUS_SLEEP) ? 1 : 0,
         (s.control_status & STATUS_HIBERNATE) ? 1 : 0,
         s.flags,
         bq27441_flags_to_text(s.flags),
         flags_has(s.flags, BQ27441_FLAG_ITPOR) ? 1 : 0,
         flags_has(s.flags, BQ27441_FLAG_CFGUPMODE) ? 1 : 0,
         flags_has(s.flags, BQ27441_FLAG_BAT_DET) ? 1 : 0,
         s.voltage_mv,
         s.average_current_ma,
         s.soc_percent,
         s.remaining_capacity_mah,
         s.full_charge_capacity_mah,
         s.design_capacity_mah,
         bq27441_target_design_capacity_mah());
}

void log_diagnostic_delta(const BqDiagnosticSnapshot& before,
                          const BqDiagnosticSnapshot& after)
{
    const int soc_delta = static_cast<int>(after.soc_percent) - static_cast<int>(before.soc_percent);
    const int rm_delta = static_cast<int>(after.remaining_capacity_mah) -
                         static_cast<int>(before.remaining_capacity_mah);
    const int fcc_delta = static_cast<int>(after.full_charge_capacity_mah) -
                          static_cast<int>(before.full_charge_capacity_mah);
    LOGI("[BQ27441][重启诊断][配置变化] SOC=%+d%% RM=%+dmAh FCC=%+dmAh；自动流程未使用 SOFT_RESET",
         soc_delta,
         rm_delta,
         fcc_delta);
}

bool enter_config_update(bool* was_sealed, bool already_in_cfgupdate)
{
    if (was_sealed) *was_sealed = false;

    uint16_t status = 0;
    if (control_word(CTRL_CONTROL_STATUS, &status) && flags_has(status, STATUS_SS)) {
        if (was_sealed) *was_sealed = true;
        if (!execute_control(CTRL_UNSEAL_KEY)) return false;
        delay(2);
        if (!execute_control(CTRL_UNSEAL_KEY)) return false;
        delay(10);
    }

    if (already_in_cfgupdate) {
        LOGW("[BQ27441][配置模式] 检测到上次遗留 CFGUPMODE，复用当前配置模式");
        return true;
    }

    if (!execute_control(CTRL_SET_CFGUPDATE)) return false;
    return wait_for_cfgupdate(true, 1000);
}

bool exit_config_update(bool was_sealed, bool resimulate)
{
    const uint16_t preferred = resimulate ? CTRL_EXIT_RESIM : CTRL_EXIT_CFGUPDATE;
    bool ok = execute_control(preferred);
    uint16_t used = preferred;

    // EXIT_RESIM 失败时只回退到无重模拟的退出命令，自动流程绝不发送 SOFT_RESET。
    if (!ok && preferred != CTRL_EXIT_CFGUPDATE) {
        ok = execute_control(CTRL_EXIT_CFGUPDATE);
        used = CTRL_EXIT_CFGUPDATE;
    }
    if (!ok) return false;

    const bool left = wait_for_cfgupdate(false, 2000);
    if (was_sealed) {
        (void)execute_control(CTRL_SEALED);
    }

    LOGI("[BQ27441][配置退出] 命令=%s(0x%04X) left=%d；未发送 SOFT_RESET",
         used == CTRL_EXIT_RESIM ? "EXIT_RESIM" : "EXIT_CFGUPDATE",
         used,
         left ? 1 : 0);
    return left;
}

bool configure_design_capacity_data_memory(uint16_t flags_hint, BqConfigTrigger trigger)
{
    const uint32_t now = millis();
    if (trigger != BqConfigTrigger::User &&
        s_next_config_attempt_ms != 0 &&
        static_cast<int32_t>(now - s_next_config_attempt_ms) < 0) {
        return false;
    }

    s_config_attempted_this_boot = true;

    load_design_capacity_pref_once();
    const uint16_t target_capacity = s_target_design_capacity_mah;
    const uint16_t target_energy = design_energy_mwh_for_capacity(target_capacity);
    const uint16_t target_taper_rate = taper_rate_for_capacity(target_capacity);

    BqDiagnosticSnapshot before{};
    (void)capture_diagnostic_snapshot(&before);
    if ((before.valid_mask & DIAG_FLAGS) == 0) before.flags = flags_hint;
    log_diagnostic_snapshot("配置前", trigger, before);

    const bool got_design_capacity = (before.valid_mask & DIAG_DESIGN) != 0;
    const uint16_t design_capacity = before.design_capacity_mah;
    if (got_design_capacity) {
        s_design_capacity_mah = design_capacity;
    }

    const uint16_t effective_flags = (before.valid_mask & DIAG_FLAGS) ? before.flags : flags_hint;
    const bool itpor = flags_has(effective_flags, BQ27441_FLAG_ITPOR);
    const bool cfgupmode = flags_has(effective_flags, BQ27441_FLAG_CFGUPMODE);

    // 读取 DesignCapacity 失败时绝不能把“未知”当成“不匹配”并贸然进入配置模式。
    if (!got_design_capacity) {
        s_config_ok_this_boot = false;
        s_next_config_attempt_ms = now + BQ27441_CONFIG_RETRY_MS;
        LOGW("[BQ27441][配置决策] 延后：无法读取 DesignCapacity，ITPOR=%d CFGUP=%d err=%u；未进入 CFGUPDATE，未发送 SOFT_RESET",
             itpor ? 1 : 0,
             cfgupmode ? 1 : 0,
             s_last_i2c_error);
        return false;
    }

    const bool capacity_mismatch = design_capacity != target_capacity;
    const bool need_config = itpor || cfgupmode || capacity_mismatch;
    if (!need_config) {
        LOGI("[BQ27441][配置决策] 跳过：ITPOR=0 CFGUP=0 DesignCapacity=%umAh 与目标一致；未进入 CFGUPDATE，未发送 SOFT_RESET",
             design_capacity);
        s_config_ok_this_boot = true;
        s_next_config_attempt_ms = 0;
        return true;
    }

    char reason[80] = {0};
    snprintf(reason, sizeof(reason), "%s%s%s",
             itpor ? "ITPOR " : "",
             cfgupmode ? "CFGUP残留 " : "",
             capacity_mismatch ? "容量不匹配" : "");
    LOGW("[BQ27441][配置决策] 执行：来源=%s 原因=%s DesignCapacity=%umAh->%umAh",
         config_trigger_name(trigger),
         reason,
         design_capacity,
         target_capacity);

    bool was_sealed = false;
    if (!enter_config_update(&was_sealed, cfgupmode)) {
        LOGW("[BQ27441] 进入配置模式失败 err=%u", s_last_i2c_error);
        s_config_ok_this_boot = false;
        s_next_config_attempt_ms = millis() + BQ27441_CONFIG_RETRY_MS;
        return false;
    }

    bool ok = true;
    i2c_bus_lock();
    ok &= write_extended_u16_be_locked(CLASS_STATE, STATE_OFF_DESIGN_CAPACITY, target_capacity);
    ok &= write_extended_u16_be_locked(CLASS_STATE, STATE_OFF_DESIGN_ENERGY, target_energy);
    ok &= write_extended_u16_be_locked(CLASS_STATE, STATE_OFF_TERMINATE_VOLTAGE, BQ27441_TERMINATE_VOLTAGE_MV);
    ok &= write_extended_u16_be_locked(CLASS_STATE, STATE_OFF_TAPER_RATE, target_taper_rate);

    ok &= write_extended_u8_locked(CLASS_DISCHARGE, DISCHARGE_OFF_SOC1_SET, BQ27441_SOC1_SET_PERCENT);
    ok &= write_extended_u8_locked(CLASS_DISCHARGE, DISCHARGE_OFF_SOC1_CLEAR, BQ27441_SOC1_CLEAR_PERCENT);
    ok &= write_extended_u8_locked(CLASS_DISCHARGE, DISCHARGE_OFF_SOCF_SET, BQ27441_SOCF_SET_PERCENT);
    ok &= write_extended_u8_locked(CLASS_DISCHARGE, DISCHARGE_OFF_SOCF_CLEAR, BQ27441_SOCF_CLEAR_PERCENT);

    uint16_t opconfig = 0;
    if (read_word_at_locked(BQ27441_ADDR, CMD_EXT_OPCONFIG, &opconfig)) {
        uint16_t next_opconfig = opconfig;
        next_opconfig |= OPCONFIG_BATLOWEN;
        next_opconfig &= (uint16_t)~OPCONFIG_GPIOPOL;
        if (next_opconfig != opconfig) {
            ok &= write_extended_u16_be_locked(CLASS_REGISTERS, REGISTERS_OFF_OPCONFIG, next_opconfig);
        }
    } else {
        ok = false;
    }
    i2c_bus_unlock();

    // 参数全部写入成功时使用 EXIT_RESIM：更新计算但不做 OCV 测量。
    // 写入失败时仅退出配置模式，避免对部分配置执行重模拟。
    const bool exited = exit_config_update(was_sealed, ok);
    ok = ok && exited;

    delay(120);

    uint16_t new_design_capacity = 0;
    if (read_word(CMD_EXT_DESIGN_CAPACITY, &new_design_capacity)) {
        s_design_capacity_mah = new_design_capacity;
    }

    s_config_ok_this_boot = ok && new_design_capacity == target_capacity;

    BqDiagnosticSnapshot after{};
    (void)capture_diagnostic_snapshot(&after);
    log_diagnostic_snapshot("配置后", trigger, after);
    log_diagnostic_delta(before, after);

    if (s_config_ok_this_boot) {
        s_next_config_attempt_ms = 0;
        LOGI("[BQ27441] 电池参数已配置：容量=%umAh 能量=%umWh 截止=%umV Taper=%u SOC1=%u/%u SOCF=%u/%u GPOUT=低有效",
             (unsigned)target_capacity,
             (unsigned)target_energy,
             (unsigned)BQ27441_TERMINATE_VOLTAGE_MV,
             (unsigned)target_taper_rate,
             (unsigned)BQ27441_SOC1_SET_PERCENT,
             (unsigned)BQ27441_SOC1_CLEAR_PERCENT,
             (unsigned)BQ27441_SOCF_SET_PERCENT,
             (unsigned)BQ27441_SOCF_CLEAR_PERCENT);
    } else {
        s_next_config_attempt_ms = millis() + BQ27441_CONFIG_RETRY_MS;
        LOGW("[BQ27441] 电池参数配置失败 ok=%d exited=%d design=%u err=%u",
             ok ? 1 : 0,
             exited ? 1 : 0,
             new_design_capacity,
             s_last_i2c_error);
    }
    return s_config_ok_this_boot;
}

}

bool bq27441_begin()
{
    load_design_capacity_pref_once();
    if (!i2c_ready_for_bq()) return false;

    const uint32_t now = millis();
    if (s_next_begin_attempt_ms != 0 &&
        static_cast<int32_t>(now - s_next_begin_attempt_ms) < 0) {
        return false;
    }

    s_ready = false;

    uint16_t device_type = 0;
    uint16_t control_status = 0;
    uint16_t voltage_mv = 0;
    uint16_t flags = 0;

    if (!detect_bq27441(&device_type, &control_status, &voltage_mv, &flags)) {
        s_next_begin_attempt_ms = now + (s_ever_ready
            ? BQ27441_RETRY_AFTER_RUNTIME_FAILURE_MS
            : BQ27441_RETRY_AFTER_MISSING_MS);
        return false;
    }

    (void)configure_design_capacity_data_memory(flags, BqConfigTrigger::Startup);

    (void)read_word(CMD_FLAGS, &flags);
    (void)read_word(CMD_EXT_DESIGN_CAPACITY, &s_design_capacity_mah);

    s_ready = true;
    s_ever_ready = true;
    s_scan_done_this_boot = true;
    s_next_begin_attempt_ms = 0;
    s_seen_bus_generation = i2c_bus_generation();
    s_consecutive_mandatory_read_failures = 0;
    s_consecutive_runtime_read_failures = 0;
    s_missing_warned_once = false;
    LOGI("[BQ27441] 初始化成功：地址=0x55 device=0x%04X control=0x%04X voltage=%umV flags=0x%04X[%s] design=%umAh",
         device_type,
         control_status,
         voltage_mv,
         flags,
         bq27441_flags_to_text(flags),
         s_design_capacity_mah);
    return true;
}

bool bq27441_is_ready()
{
    return s_ready;
}

uint8_t bq27441_last_i2c_error()
{
    return s_last_i2c_error;
}

uint16_t bq27441_design_capacity_mah()
{
    // 菜单/UI 只读取缓存，避免每次绘制状态页都访问 I2C。
    return s_design_capacity_mah;
}

uint16_t bq27441_target_design_capacity_mah()
{
    load_design_capacity_pref_once();
    return s_target_design_capacity_mah;
}

bool bq27441_set_design_capacity_mah(uint16_t capacity_mah)
{
    const uint16_t target = clamp_design_capacity_mah(capacity_mah);

    Preferences pref;
    if (!pref.begin(BQ27441_PREFS_NAMESPACE, false)) {
        LOGW("[BQ27441] 无法打开容量配置 NVS");
        return false;
    }
    const size_t written = pref.putUShort(BQ27441_PREFS_CAPACITY_KEY, target);
    pref.end();
    if (written != sizeof(uint16_t)) {
        LOGW("[BQ27441] 保存目标容量失败：%umAh", (unsigned)target);
        return false;
    }

    s_capacity_pref_loaded = true;
    s_target_design_capacity_mah = target;
    s_config_attempted_this_boot = false;
    s_config_ok_this_boot = false;
    s_next_config_attempt_ms = 0;

    if (!s_ready) {
        LOGI("[BQ27441] 已保存目标容量=%umAh，电量计恢复后应用", (unsigned)target);
        return true;
    }

    const bool applied = configure_design_capacity_data_memory(0, BqConfigTrigger::User);
    if (!applied) {
        // NVS 已保存成功；保留后续重试机会，不让一次瞬时 I2C 失败丢失用户设置。
        s_config_attempted_this_boot = false;
        LOGW("[BQ27441] 目标容量已保存=%umAh，本次应用失败，后续采样时重试",
             (unsigned)target);
        return true;
    }

    LOGI("[BQ27441] 自定义容量保存并应用：目标=%umAh 结果=1",
         (unsigned)target);
    return true;
}

bool bq27441_configure_if_needed(uint16_t flags_hint)
{
    if (!i2c_ready_for_bq()) return false;
    return configure_design_capacity_data_memory(flags_hint, BqConfigTrigger::Runtime);
}

const char* bq27441_flags_to_text(uint16_t flags)
{
    static char buf[160];
    size_t pos = 0;
    auto add = [&](const char* label) {
        if (pos >= sizeof(buf) - 1) return;
        if (pos != 0 && pos + 1 < sizeof(buf)) buf[pos++] = ' ';
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s", label);
    };

    if (flags_has(flags, BQ27441_FLAG_FC)) add("满电");
    if (flags_has(flags, BQ27441_FLAG_CHG)) add("充电允许");
    if (flags_has(flags, BQ27441_FLAG_DSG)) add("放电");
    if (flags_has(flags, BQ27441_FLAG_BAT_DET)) add("电池在位");
    if (flags_has(flags, BQ27441_FLAG_SOC1)) add("低电");
    if (flags_has(flags, BQ27441_FLAG_SOCF)) add("极低");
    if (flags_has(flags, BQ27441_FLAG_OCVTAKEN)) add("OCV已取");
    if (flags_has(flags, BQ27441_FLAG_ITPOR)) add("需配置");
    if (flags_has(flags, BQ27441_FLAG_CFGUPMODE)) add("配置中");

    if (pos == 0) {
        snprintf(buf, sizeof(buf), "无");
    }
    return buf;
}

bool bq27441_read_runtime(Bq27441RuntimeSample* out)
{
    if (!out) return false;
    *out = Bq27441RuntimeSample{};
    if (!i2c_ready_for_bq()) return false;

    // I2C 总线恢复后允许立即重新初始化。
    const uint32_t generation = i2c_bus_generation();
    if (generation != s_seen_bus_generation) {
        s_seen_bus_generation = generation;
        s_next_begin_attempt_ms = 0;
        s_next_config_attempt_ms = 0;
    }

    if (!s_ready) {
        (void)bq27441_begin();
    }
    if (!s_ready) return false;

    uint16_t remaining_capacity = 0;
    uint16_t current_raw = 0;

    i2c_bus_lock();
    const bool got_capacity =
        read_word_at_locked(BQ27441_ADDR, CMD_REMAINING_CAPACITY, &remaining_capacity);
    const bool got_current = got_capacity &&
        read_word_at_locked(BQ27441_ADDR, CMD_AVERAGE_CURRENT, &current_raw);
    i2c_bus_unlock();

    if (!got_capacity || !got_current) {
        if (s_consecutive_runtime_read_failures < 255) {
            ++s_consecutive_runtime_read_failures;
        }
        if (warn_due(s_last_runtime_warn_ms, BQ27441_WARN_INTERVAL_MS)) {
            LOGW("[BQ27441] 续航轻量采样失败：命令=0x%02X err=%u 连续失败=%u",
                 s_last_failed_cmd,
                 s_last_i2c_error,
                 s_consecutive_runtime_read_failures);
        }
        if (s_consecutive_runtime_read_failures >= 3) {
            s_ready = false;
            s_next_begin_attempt_ms = millis() + BQ27441_RETRY_AFTER_RUNTIME_FAILURE_MS;
        }
        return false;
    }

    s_consecutive_runtime_read_failures = 0;
    out->valid = true;
    out->remaining_capacity_mah = remaining_capacity;
    out->average_current_ma = static_cast<int16_t>(current_raw);
    return true;
}

bool bq27441_read(Bq27441Sample* out)
{
    if (!out) return false;
    if (!i2c_ready_for_bq()) return false;

    Bq27441Sample s{};

    // 总线恢复后允许立即重试一次，不必等待原来的离线退避截止时间。
    const uint32_t generation = i2c_bus_generation();
    if (generation != s_seen_bus_generation) {
        s_seen_bus_generation = generation;
        s_next_begin_attempt_ms = 0;
        s_next_config_attempt_ms = 0;
    }

    if (!s_ready) {
        (void)bq27441_begin();
    }
    if (!s_ready) {
        *out = s;
        return false;
    }

    uint16_t current_raw = 0;
    uint8_t failed_cmd = 0;
    uint8_t failed_err = 0;

    auto read_required = [&](uint8_t cmd, uint16_t* value) -> bool {
        if (read_word(cmd, value)) return true;
        failed_cmd = cmd;
        failed_err = s_last_i2c_error;
        return false;
    };

    const bool mandatory_ok =
        read_required(CMD_VOLTAGE, &s.voltage_mv) &&
        read_required(CMD_FLAGS, &s.flags) &&
        read_required(CMD_STATE_OF_CHARGE, &s.soc_percent);

    if (!mandatory_ok) {
        if (s_consecutive_mandatory_read_failures < 255) {
            ++s_consecutive_mandatory_read_failures;
        }
        if (warn_due(s_last_read_warn_ms, BQ27441_WARN_INTERVAL_MS)) {
            LOGW("[BQ27441] 读取失败：必要命令=0x%02X err=%u 连续失败=%u ready=%d",
                 failed_cmd,
                 failed_err,
                 s_consecutive_mandatory_read_failures,
                 s_ready ? 1 : 0);
        }
        if (s_consecutive_mandatory_read_failures >= 3) {
            s_ready = false;
            s_next_begin_attempt_ms = millis() + BQ27441_RETRY_AFTER_RUNTIME_FAILURE_MS;
        }
        *out = Bq27441Sample{};
        return false;
    }

    s_consecutive_mandatory_read_failures = 0;
    s_consecutive_runtime_read_failures = 0;

    if (flags_has(s.flags, BQ27441_FLAG_ITPOR) || s_design_capacity_mah != bq27441_target_design_capacity_mah()) {
        (void)configure_design_capacity_data_memory(s.flags, BqConfigTrigger::Runtime);
    }

    uint8_t optional_failed_cmd = 0;
    uint8_t optional_failed_err = 0;
    auto read_optional = [&](uint8_t cmd, uint16_t* value) -> bool {
        if (read_word(cmd, value)) return true;
        if (optional_failed_cmd == 0) {
            optional_failed_cmd = cmd;
            optional_failed_err = s_last_i2c_error;
        }
        return false;
    };

    (void)read_optional(CMD_REMAINING_CAPACITY, &s.remaining_capacity_mah);
    (void)read_optional(CMD_FULL_CHARGE_CAPACITY, &s.full_charge_capacity_mah);
    (void)read_optional(CMD_EXT_DESIGN_CAPACITY, &s.design_capacity_mah);
    (void)read_optional(CMD_AVERAGE_CURRENT, &current_raw);
#if BQ27441_READ_SOH
    (void)read_optional(CMD_STATE_OF_HEALTH, &s.state_of_health_percent);
#endif

    if (optional_failed_cmd != 0 && warn_due(s_last_read_warn_ms, BQ27441_WARN_INTERVAL_MS)) {
        LOGW("[BQ27441] 附加字段读取失败：命令=0x%02X err=%u，核心电量仍有效",
             optional_failed_cmd,
             optional_failed_err);
    }

    if (s.design_capacity_mah != 0) {
        s_design_capacity_mah = s.design_capacity_mah;
    } else {
        s.design_capacity_mah = s_design_capacity_mah;
    }

    s.average_current_ma = (int16_t)current_raw;
    if (s.soc_percent > 100) s.soc_percent = 100;
    if (s.state_of_health_percent > 100) s.state_of_health_percent = 100;
    s.valid = true;

    *out = s;
    return true;
}
