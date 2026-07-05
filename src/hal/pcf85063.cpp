#include "hal/pcf85063.h"

#include <Wire.h>
#include <stdio.h>

#include "board/board_pins.h"
#include "hal/i2c_bus_lock.h"
#include "utils/log.h"

namespace {

constexpr uint8_t PCF85063_ADDR = 0x51;

constexpr uint8_t REG_CONTROL_2 = 0x01;
constexpr uint8_t REG_SECONDS = 0x04;
constexpr uint8_t REG_MINUTES = 0x05;
constexpr uint8_t REG_HOURS = 0x06;
constexpr uint8_t REG_DAYS = 0x07;
constexpr uint8_t REG_WEEKDAYS = 0x08;
constexpr uint8_t REG_MONTHS = 0x09;
constexpr uint8_t REG_YEARS = 0x0A;
constexpr uint8_t REG_SECOND_ALARM = 0x0B;

constexpr uint8_t CTRL2_AIE = 0x80;
constexpr uint8_t CTRL2_AF = 0x40;
constexpr uint8_t CTRL2_TF = 0x08;

constexpr uint8_t SECONDS_OS = 0x80;
constexpr uint8_t ALARM_DISABLE = 0x80;

bool s_ready = false;
uint8_t s_last_i2c_error = 0;
bool s_boot_alarm_pending = false;
uint8_t s_last_control2 = 0;
Pcf85063DateTime s_last_time{};

bool i2c_ready_for_rtc()
{
    if (i2c_bus_is_ready()) return true;
    s_last_i2c_error = 0xFE;
    return false;
}

uint8_t bcd_to_bin(uint8_t bcd)
{
    return static_cast<uint8_t>(((bcd >> 4) * 10) + (bcd & 0x0F));
}

uint8_t bin_to_bcd(uint8_t bin)
{
    return static_cast<uint8_t>(((bin / 10) << 4) | (bin % 10));
}

bool valid_datetime_fields(const Pcf85063DateTime& t)
{
    return t.year >= 2000 && t.year <= 2099 &&
           t.month >= 1 && t.month <= 12 &&
           t.day >= 1 && t.day <= 31 &&
           t.weekday <= 6 &&
           t.hour <= 23 &&
           t.minute <= 59 &&
           t.second <= 59;
}

static uint32_t seconds_of_day(const Pcf85063DateTime& t)
{
    return (uint32_t)t.hour * 3600UL + (uint32_t)t.minute * 60UL + (uint32_t)t.second;
}

static void time_from_seconds_of_day(uint32_t sod, uint8_t& hour, uint8_t& minute, uint8_t& second)
{
    sod %= 24UL * 3600UL;
    hour = (uint8_t)(sod / 3600UL);
    sod %= 3600UL;
    minute = (uint8_t)(sod / 60UL);
    second = (uint8_t)(sod % 60UL);
}

bool read_bytes_locked(uint8_t reg, uint8_t* out, size_t len)
{
    if (!out || len == 0) return false;

    Wire.beginTransmission(PCF85063_ADDR);
    Wire.write(reg);
    uint8_t err = Wire.endTransmission(true);
    s_last_i2c_error = err;
    if (err != 0) return false;

    delayMicroseconds(300);

    const uint8_t n = Wire.requestFrom((int)PCF85063_ADDR, (int)len, true);
    if (n != len || Wire.available() < (int)len) {
        s_last_i2c_error = 0xF0 | n;
        return false;
    }

    for (size_t i = 0; i < len; ++i) {
        out[i] = Wire.read();
    }
    s_last_i2c_error = 0;
    return true;
}

bool write_bytes_locked(uint8_t reg, const uint8_t* data, size_t len)
{
    if (!data && len > 0) return false;

    Wire.beginTransmission(PCF85063_ADDR);
    Wire.write(reg);
    for (size_t i = 0; i < len; ++i) {
        Wire.write(data[i]);
    }
    const uint8_t err = Wire.endTransmission(true);
    s_last_i2c_error = err;
    return err == 0;
}

bool read_u8(uint8_t reg, uint8_t* out)
{
    if (!out || !i2c_ready_for_rtc()) return false;
    i2c_bus_lock();
    const bool ok = read_bytes_locked(reg, out, 1);
    i2c_bus_unlock();
    return ok;
}

bool write_u8(uint8_t reg, uint8_t value)
{
    if (!i2c_ready_for_rtc()) return false;
    i2c_bus_lock();
    const bool ok = write_bytes_locked(reg, &value, 1);
    i2c_bus_unlock();
    return ok;
}

bool probe_rtc()
{
    if (!i2c_ready_for_rtc()) return false;
    i2c_bus_lock();
    Wire.beginTransmission(PCF85063_ADDR);
    const uint8_t err = Wire.endTransmission(true);
    i2c_bus_unlock();
    s_last_i2c_error = err;
    return err == 0;
}

bool read_control2(uint8_t* out)
{
    return read_u8(REG_CONTROL_2, out);
}

bool write_control2(uint8_t value)
{
    return write_u8(REG_CONTROL_2, value);
}

void configure_rtc_int_input()
{
#ifdef PIN_RTC_INT
    if (PIN_RTC_INT >= 0) {
        pinMode(PIN_RTC_INT, INPUT_PULLUP);
    }
#endif
}

bool read_rtc_int_level(bool* out)
{
    if (!out) return false;
#ifdef PIN_RTC_INT
    if (PIN_RTC_INT >= 0) {
        configure_rtc_int_input();
        *out = digitalRead(PIN_RTC_INT) == HIGH;
        return true;
    }
#endif
    return false;
}

}

bool pcf85063_begin(bool clear_alarm_flag_on_boot)
{
    s_ready = false;
    configure_rtc_int_input();

    if (!i2c_ready_for_rtc()) {
        return false;
    }

    if (!probe_rtc()) {
        LOGW("[PCF85063] 初始化失败：地址0x%02X无ACK err=%u", PCF85063_ADDR, s_last_i2c_error);
        return false;
    }

    uint8_t ctrl2 = 0;
    if (!read_control2(&ctrl2)) {
        LOGW("[PCF85063] 初始化失败：无法读取Control_2 err=%u", s_last_i2c_error);
        return false;
    }

    s_last_control2 = ctrl2;
    s_boot_alarm_pending = (ctrl2 & CTRL2_AF) != 0;

    Pcf85063DateTime now{};
    const bool time_ok = pcf85063_read_time(&now);

    if (clear_alarm_flag_on_boot && s_boot_alarm_pending) {
        // RTC_INT + AO3401A 定时开机属于一次性触发：开机后必须同时关闭 AIE 和清 AF。
        // 只清 AF 会留下 Control_2=0x80，菜单显示“已启用”，并可能在下一天同一时间再次触发。
        if (pcf85063_disable_alarm()) {
            LOGI("[PCF85063] 检测到RTC闹钟开机，已关闭一次性闹钟");
        } else if (pcf85063_clear_alarm_flag()) {
            LOGW("[PCF85063] 检测到RTC闹钟开机，关闭闹钟失败，仅清除AF err=%u", s_last_i2c_error);
        } else {
            LOGW("[PCF85063] 检测到RTC闹钟开机，但清除AF失败 err=%u", s_last_i2c_error);
        }
    }

    uint8_t current_ctrl2 = 0;
    if (read_control2(&current_ctrl2)) {
        ctrl2 = current_ctrl2;
        s_last_control2 = ctrl2;
    }

    s_ready = true;

    bool int_level = true;
    const bool int_known = read_rtc_int_level(&int_level);

    LOGI("[PCF85063] 初始化成功：地址=0x%02X time_valid=%d os=%d alarm_pending=%d aie=%d int=%s%s ctrl2=0x%02X%s%s",
         PCF85063_ADDR,
         (time_ok && now.valid) ? 1 : 0,
         now.oscillator_stopped ? 1 : 0,
         s_boot_alarm_pending ? 1 : 0,
         (ctrl2 & CTRL2_AIE) ? 1 : 0,
         int_known ? (int_level ? "高" : "低") : "未接",
         int_known ? "" : "",
         ctrl2,
         time_ok ? " time=" : "",
         time_ok ? pcf85063_datetime_to_text(now) : "");

    return true;
}

bool pcf85063_is_ready()
{
    return s_ready;
}

uint8_t pcf85063_last_i2c_error()
{
    return s_last_i2c_error;
}

bool pcf85063_read_time(Pcf85063DateTime* out)
{
    if (!out) return false;
    *out = Pcf85063DateTime{};
    if (!i2c_ready_for_rtc()) return false;

    uint8_t data[7] = {0};
    i2c_bus_lock();
    const bool ok = read_bytes_locked(REG_SECONDS, data, sizeof(data));
    i2c_bus_unlock();
    if (!ok) return false;

    out->oscillator_stopped = (data[0] & SECONDS_OS) != 0;
    out->second = bcd_to_bin(data[0] & 0x7F);
    out->minute = bcd_to_bin(data[1] & 0x7F);
    out->hour = bcd_to_bin(data[2] & 0x3F);
    out->day = bcd_to_bin(data[3] & 0x3F);
    out->weekday = bcd_to_bin(data[4] & 0x07);
    out->month = bcd_to_bin(data[5] & 0x1F);
    out->year = static_cast<uint16_t>(2000 + bcd_to_bin(data[6]));
    out->valid = valid_datetime_fields(*out) && !out->oscillator_stopped;

    s_last_time = *out;
    return true;
}

bool pcf85063_set_time(const Pcf85063DateTime& t)
{
    if (!valid_datetime_fields(t) || !i2c_ready_for_rtc()) return false;

    uint8_t data[7] = {
        bin_to_bcd(t.second),
        bin_to_bcd(t.minute),
        bin_to_bcd(t.hour),
        bin_to_bcd(t.day),
        bin_to_bcd(t.weekday),
        bin_to_bcd(t.month),
        bin_to_bcd(static_cast<uint8_t>(t.year - 2000)),
    };

    i2c_bus_lock();
    const bool ok = write_bytes_locked(REG_SECONDS, data, sizeof(data));
    i2c_bus_unlock();

    if (ok) {
        s_last_time = t;
        s_last_time.valid = true;
        s_last_time.oscillator_stopped = false;
        s_ready = true;
        LOGI("[PCF85063] 时间已设置：%s", pcf85063_datetime_to_text(s_last_time));
    }
    return ok;
}

bool pcf85063_read_status(Pcf85063Status* out)
{
    if (!out) return false;
    *out = Pcf85063Status{};
    out->ready = s_ready;

    uint8_t ctrl2 = 0;
    if (!read_control2(&ctrl2)) {
        return false;
    }

    out->control2 = ctrl2;
    out->alarm_pending = (ctrl2 & CTRL2_AF) != 0;
    out->alarm_enabled = (ctrl2 & CTRL2_AIE) != 0;
    out->timer_pending = (ctrl2 & CTRL2_TF) != 0;
    s_last_control2 = ctrl2;

    bool int_level = true;
    out->rtc_int_level_known = read_rtc_int_level(&int_level);
    out->rtc_int_level = int_level;

    Pcf85063DateTime t{};
    if (pcf85063_read_time(&t)) {
        out->time = t;
        out->time_valid = t.valid;
        out->oscillator_stopped = t.oscillator_stopped;
    }

    return true;
}

bool pcf85063_alarm_pending()
{
    uint8_t ctrl2 = 0;
    if (!read_control2(&ctrl2)) return false;
    s_last_control2 = ctrl2;
    return (ctrl2 & CTRL2_AF) != 0;
}

bool pcf85063_clear_alarm_flag()
{
    uint8_t ctrl2 = 0;
    if (!read_control2(&ctrl2)) return false;
    ctrl2 &= static_cast<uint8_t>(~CTRL2_AF);
    const bool ok = write_control2(ctrl2);
    if (ok) s_last_control2 = ctrl2;
    return ok;
}

bool pcf85063_clear_timer_flag()
{
    uint8_t ctrl2 = 0;
    if (!read_control2(&ctrl2)) return false;
    ctrl2 &= static_cast<uint8_t>(~CTRL2_TF);
    const bool ok = write_control2(ctrl2);
    if (ok) s_last_control2 = ctrl2;
    return ok;
}

bool pcf85063_clear_interrupt_flags()
{
    uint8_t ctrl2 = 0;
    if (!read_control2(&ctrl2)) return false;
    ctrl2 &= static_cast<uint8_t>(~(CTRL2_AF | CTRL2_TF));
    const bool ok = write_control2(ctrl2);
    if (ok) s_last_control2 = ctrl2;
    return ok;
}

bool pcf85063_set_alarm_after_seconds(uint32_t seconds)
{
    if (seconds == 0 || seconds > 24UL * 3600UL || !i2c_ready_for_rtc()) return false;

    Pcf85063DateTime now{};
    if (!pcf85063_read_time(&now) || !now.valid) {
        LOGW("[PCF85063] 设置闹钟失败：RTC时间无效");
        return false;
    }

    uint8_t target_hour = 0;
    uint8_t target_minute = 0;
    uint8_t target_second = 0;
    time_from_seconds_of_day(seconds_of_day(now) + seconds, target_hour, target_minute, target_second);

    const uint8_t alarm_regs[5] = {
        bin_to_bcd(target_second),
        bin_to_bcd(target_minute),
        bin_to_bcd(target_hour),
        ALARM_DISABLE,
        ALARM_DISABLE,
    };

    uint8_t ctrl2 = 0;
    if (!read_control2(&ctrl2)) return false;
    ctrl2 &= static_cast<uint8_t>(~CTRL2_AF);

    i2c_bus_lock();
    const bool alarm_ok = write_bytes_locked(REG_SECOND_ALARM, alarm_regs, sizeof(alarm_regs));
    i2c_bus_unlock();
    if (!alarm_ok) {
        LOGW("[PCF85063] 设置闹钟失败：写闹钟寄存器失败 err=%u", s_last_i2c_error);
        return false;
    }

    ctrl2 |= CTRL2_AIE;
    if (!write_control2(ctrl2)) {
        LOGW("[PCF85063] 设置闹钟失败：启用AIE失败 err=%u", s_last_i2c_error);
        return false;
    }

    s_last_control2 = ctrl2;
    LOGI("[PCF85063] 已设置测试闹钟：%02u:%02u:%02u 后约%lu秒触发",
         (unsigned)target_hour,
         (unsigned)target_minute,
         (unsigned)target_second,
         (unsigned long)seconds);
    return true;
}

bool pcf85063_set_test_alarm_after_one_minute()
{
    return pcf85063_set_alarm_after_seconds(60);
}

bool pcf85063_disable_alarm()
{
    if (!i2c_ready_for_rtc()) return false;

    const uint8_t alarm_regs[5] = {
        ALARM_DISABLE,
        ALARM_DISABLE,
        ALARM_DISABLE,
        ALARM_DISABLE,
        ALARM_DISABLE,
    };

    uint8_t ctrl2 = 0;
    if (!read_control2(&ctrl2)) return false;
    ctrl2 &= static_cast<uint8_t>(~(CTRL2_AIE | CTRL2_AF));

    i2c_bus_lock();
    const bool alarm_ok = write_bytes_locked(REG_SECOND_ALARM, alarm_regs, sizeof(alarm_regs));
    i2c_bus_unlock();
    if (!alarm_ok) {
        LOGW("[PCF85063] 清除闹钟失败：写闹钟寄存器失败 err=%u", s_last_i2c_error);
        return false;
    }

    const bool ok = write_control2(ctrl2);
    if (ok) {
        s_last_control2 = ctrl2;
        LOGI("[PCF85063] RTC闹钟已关闭");
    }
    return ok;
}

bool pcf85063_boot_alarm_was_pending()
{
    return s_boot_alarm_pending;
}

const char* pcf85063_status_label()
{
    if (!s_ready) return "ERR";
    if (s_last_time.oscillator_stopped) return "未设置";
    if (!s_last_time.valid) return "待校时";
    return "OK";
}

const char* pcf85063_alarm_status_label()
{
    uint8_t ctrl2 = 0;
    if (!read_control2(&ctrl2)) return "未知";
    s_last_control2 = ctrl2;
    if (ctrl2 & CTRL2_AF) return "已触发";
    if (ctrl2 & CTRL2_AIE) return "已启用";
    return "关闭";
}

const char* pcf85063_datetime_to_text(const Pcf85063DateTime& t)
{
    static char buf[24];
    if (!t.valid && t.oscillator_stopped) {
        return "晶振停止";
    }
    if (!valid_datetime_fields(t)) {
        return "无效";
    }
    snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u",
             static_cast<unsigned>(t.year),
             static_cast<unsigned>(t.month),
             static_cast<unsigned>(t.day),
             static_cast<unsigned>(t.hour),
             static_cast<unsigned>(t.minute),
             static_cast<unsigned>(t.second));
    return buf;
}