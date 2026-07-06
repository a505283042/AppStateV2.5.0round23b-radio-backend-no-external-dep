#pragma once

#include <Arduino.h>
#include <stdint.h>

struct Pcf85063DateTime {
    uint16_t year = 0;
    uint8_t month = 0;
    uint8_t day = 0;
    uint8_t weekday = 0;
    uint8_t hour = 0;
    uint8_t minute = 0;
    uint8_t second = 0;
    bool valid = false;
    bool oscillator_stopped = false;
};

struct Pcf85063Status {
    bool ready = false;
    bool time_valid = false;
    bool oscillator_stopped = false;
    bool alarm_pending = false;
    bool alarm_enabled = false;
    bool timer_pending = false;
    bool rtc_int_level_known = false;
    bool rtc_int_level = true;
    uint8_t control2 = 0;
    Pcf85063DateTime time{};
};

bool pcf85063_begin(bool clear_alarm_flag_on_boot = true);
bool pcf85063_is_ready();
uint8_t pcf85063_last_i2c_error();

bool pcf85063_read_time(Pcf85063DateTime* out);
bool pcf85063_set_time(const Pcf85063DateTime& t);

bool pcf85063_read_status(Pcf85063Status* out);
bool pcf85063_alarm_pending();
bool pcf85063_clear_alarm_flag();
bool pcf85063_clear_timer_flag();
bool pcf85063_clear_interrupt_flags();

// 闹钟字段忽略标记：传入这个值表示该字段不参与 RTC 闹钟比较。
static constexpr uint8_t PCF85063_ALARM_IGNORE = 0xFF;

// 设置 RTC 闹钟字段。
// second/minute/hour/day/weekday 可以分别指定，也可以传 PCF85063_ALARM_IGNORE 忽略。
// 至少要指定一个字段，否则函数会返回 false。
bool pcf85063_set_alarm(uint8_t second,
                        uint8_t minute,
                        uint8_t hour,
                        uint8_t day = PCF85063_ALARM_IGNORE,
                        uint8_t weekday = PCF85063_ALARM_IGNORE);

// 设置每天某个时分秒触发的 RTC 闹钟；日和星期不参与比较。
bool pcf85063_set_alarm_time_of_day(uint8_t hour, uint8_t minute, uint8_t second = 0);

bool pcf85063_set_alarm_after_seconds(uint32_t seconds);
bool pcf85063_set_test_alarm_after_one_minute();
bool pcf85063_disable_alarm();

bool pcf85063_boot_alarm_was_pending();
const char* pcf85063_status_label();
const char* pcf85063_alarm_status_label();
const char* pcf85063_datetime_to_text(const Pcf85063DateTime& t);