#include "hal/bq27441.h"

#include <Wire.h>
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
constexpr uint16_t CTRL_SOFT_RESET = 0x0042;
constexpr uint16_t CTRL_EXIT_CFGUPDATE = 0x0043;
constexpr uint16_t CTRL_UNSEAL_KEY = 0x8000;

constexpr uint16_t STATUS_SS = (1u << 13);
constexpr uint16_t OPCONFIG_GPIOPOL = (1u << 11);
constexpr uint16_t OPCONFIG_BATLOWEN = (1u << 2);

constexpr uint32_t BQ27441_WARN_INTERVAL_MS = 60UL * 1000UL;
constexpr uint32_t BQ27441_SCAN_INTERVAL_MS = 60UL * 1000UL;

#ifndef BQ27441_DESIGN_CAPACITY_MAH
#define BQ27441_DESIGN_CAPACITY_MAH 500
#endif

#ifndef BQ27441_DESIGN_ENERGY_MWH
#define BQ27441_DESIGN_ENERGY_MWH 1850
#endif

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
uint32_t s_last_failed_begin_ms = 0;
uint32_t s_last_warn_ms = 0;
uint32_t s_last_read_warn_ms = 0;
uint8_t s_consecutive_mandatory_read_failures = 0;
uint8_t s_last_failed_cmd = 0;
bool s_missing_warned_once = false;
uint16_t s_design_capacity_mah = 0;
bool s_config_attempted_this_boot = false;
bool s_config_ok_this_boot = false;

const char* known_i2c_label(uint8_t addr)
{
    switch (addr) {
        case 0x20: return "MCP23017";
        case 0x51: return "AT24C";
        case 0x55: return "BQ27441";
        case 0x5F: return "PCF85063";
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
    if (i2c_bus_is_ready()) return true;
    s_last_i2c_error = 0xFE;
    return false;
}

void log_i2c_scan_once()
{
    if (!i2c_ready_for_bq()) return;
    const uint32_t now = millis();
    if (s_last_failed_begin_ms != 0 && now - s_last_failed_begin_ms < 5000) return;
    s_last_failed_begin_ms = now;

    char buf[200];
    size_t pos = 0;
    int found = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "[BQ27441] I2C扫描：");

    i2c_bus_lock();
    for (uint8_t addr = 1; addr < 0x7F; ++addr) {
        Wire.beginTransmission(addr);
        const uint8_t err = Wire.endTransmission(true);
        if (err == 0) {
            ++found;
            const char* label = known_i2c_label(addr);
            if (pos + 16 < sizeof(buf)) {
                if (*label) {
                    pos += snprintf(buf + pos, sizeof(buf) - pos, " 0x%02X(%s)", addr, label);
                } else {
                    pos += snprintf(buf + pos, sizeof(buf) - pos, " 0x%02X", addr);
                }
            }
        }
        delay(1);
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

bool enter_config_update(bool* was_sealed)
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

    if (!execute_control(CTRL_SET_CFGUPDATE)) return false;
    return wait_for_cfgupdate(true, 1000);
}

bool exit_config_update(bool was_sealed)
{
    bool ok = execute_control(CTRL_SOFT_RESET);
    if (!ok) {
        ok = execute_control(CTRL_EXIT_CFGUPDATE);
    }
    if (!ok) return false;

    const bool left = wait_for_cfgupdate(false, 2000);
    if (was_sealed) {
        (void)execute_control(CTRL_SEALED);
    }
    return left;
}

bool configure_500mah_data_memory(uint16_t flags_hint)
{
    if (s_config_attempted_this_boot) return s_config_ok_this_boot;
    s_config_attempted_this_boot = true;

    uint16_t design_capacity = 0;
    const bool got_design_capacity = read_word(CMD_EXT_DESIGN_CAPACITY, &design_capacity);
    if (got_design_capacity) {
        s_design_capacity_mah = design_capacity;
    }

    const bool itpor = flags_has(flags_hint, BQ27441_FLAG_ITPOR);
    const bool capacity_mismatch = !got_design_capacity || design_capacity != BQ27441_DESIGN_CAPACITY_MAH;
    if (!itpor && !capacity_mismatch) {
        LOGD("[BQ27441] 配置已匹配：DesignCapacity=%umAh flags=0x%04X",
             design_capacity,
             flags_hint);
        s_config_ok_this_boot = true;
        return true;
    }

    LOGI("[BQ27441] 准备写入电池参数：DesignCapacity=%umAh->%u mAh ITPOR=%d",
         got_design_capacity ? design_capacity : 0,
         (unsigned)BQ27441_DESIGN_CAPACITY_MAH,
         itpor ? 1 : 0);

    bool was_sealed = false;
    if (!enter_config_update(&was_sealed)) {
        LOGW("[BQ27441] 进入配置模式失败 err=%u", s_last_i2c_error);
        s_config_ok_this_boot = false;
        return false;
    }

    bool ok = true;
    i2c_bus_lock();
    ok &= write_extended_u16_be_locked(CLASS_STATE, STATE_OFF_DESIGN_CAPACITY, BQ27441_DESIGN_CAPACITY_MAH);
    ok &= write_extended_u16_be_locked(CLASS_STATE, STATE_OFF_DESIGN_ENERGY, BQ27441_DESIGN_ENERGY_MWH);
    ok &= write_extended_u16_be_locked(CLASS_STATE, STATE_OFF_TERMINATE_VOLTAGE, BQ27441_TERMINATE_VOLTAGE_MV);
    ok &= write_extended_u16_be_locked(CLASS_STATE, STATE_OFF_TAPER_RATE, BQ27441_TAPER_RATE);

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

    const bool exited = exit_config_update(was_sealed);
    ok = ok && exited;

    delay(120);

    uint16_t new_design_capacity = 0;
    if (read_word(CMD_EXT_DESIGN_CAPACITY, &new_design_capacity)) {
        s_design_capacity_mah = new_design_capacity;
    }

    s_config_ok_this_boot = ok && new_design_capacity == BQ27441_DESIGN_CAPACITY_MAH;
    if (s_config_ok_this_boot) {
        LOGI("[BQ27441] 电池参数已配置：容量=%umAh 能量=%umWh 截止=%umV Taper=%u SOC1=%u/%u SOCF=%u/%u GPOUT=低有效",
             (unsigned)BQ27441_DESIGN_CAPACITY_MAH,
             (unsigned)BQ27441_DESIGN_ENERGY_MWH,
             (unsigned)BQ27441_TERMINATE_VOLTAGE_MV,
             (unsigned)BQ27441_TAPER_RATE,
             (unsigned)BQ27441_SOC1_SET_PERCENT,
             (unsigned)BQ27441_SOC1_CLEAR_PERCENT,
             (unsigned)BQ27441_SOCF_SET_PERCENT,
             (unsigned)BQ27441_SOCF_CLEAR_PERCENT);
    } else {
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
    if (!i2c_ready_for_bq()) return false;
    s_ready = false;

    uint16_t device_type = 0;
    uint16_t control_status = 0;
    uint16_t voltage_mv = 0;
    uint16_t flags = 0;

    if (!detect_bq27441(&device_type, &control_status, &voltage_mv, &flags)) {
        return false;
    }

    (void)configure_500mah_data_memory(flags);

    (void)read_word(CMD_FLAGS, &flags);
    (void)read_word(CMD_EXT_DESIGN_CAPACITY, &s_design_capacity_mah);

    s_ready = true;
    s_last_failed_begin_ms = 0;
    s_consecutive_mandatory_read_failures = 0;
    s_missing_warned_once = false;
    LOGI("[BQ27441] 初始化成功：地址=0x55 device=0x%04X control=0x%04X voltage=%umV flags=0x%04X design=%umAh",
         device_type,
         control_status,
         voltage_mv,
         flags,
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
    if (!i2c_bus_is_ready()) return s_design_capacity_mah;
    uint16_t cap = 0;
    if (read_word(CMD_EXT_DESIGN_CAPACITY, &cap)) {
        s_design_capacity_mah = cap;
    }
    return s_design_capacity_mah;
}

bool bq27441_configure_500mah_if_needed(uint16_t flags_hint)
{
    if (!i2c_ready_for_bq()) return false;
    return configure_500mah_data_memory(flags_hint);
}

const char* bq27441_flags_to_text(uint16_t flags)
{
    static char buf[80];
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

bool bq27441_read(Bq27441Sample* out)
{
    if (!out) return false;
    if (!i2c_ready_for_bq()) return false;

    Bq27441Sample s{};

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
        }
        *out = Bq27441Sample{};
        return false;
    }

    s_consecutive_mandatory_read_failures = 0;

    if (flags_has(s.flags, BQ27441_FLAG_ITPOR) || s_design_capacity_mah != BQ27441_DESIGN_CAPACITY_MAH) {
        (void)configure_500mah_data_memory(s.flags);
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