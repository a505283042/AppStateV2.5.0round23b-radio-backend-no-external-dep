#include "menu/quick_menu_page_time_alarm.h"

#include <Arduino.h>
#include <stdio.h>

#include "app_alarm.h"
#include "hal/pcf85063.h"

namespace {

#define MENU_COUNT(arr) static_cast<uint8_t>(sizeof(arr) / sizeof((arr)[0]))

static AppAlarmConfig s_draft{};
static bool s_draft_loaded = false;
static bool s_draft_dirty = false;

static void load_draft_if_needed()
{
    if (s_draft_loaded) {
        return;
    }
    s_draft = app_alarm_get_config();
    s_draft_loaded = true;
    s_draft_dirty = false;
}

static void reload_draft_from_saved()
{
    s_draft = app_alarm_get_config();
    s_draft_loaded = true;
    s_draft_dirty = false;
}

static AppAlarmConfig& draft()
{
    load_draft_if_needed();
    return s_draft;
}

static void mark_draft_dirty()
{
    s_draft_dirty = true;
}

static bool save_draft_enabled()
{
    AppAlarmConfig cfg = draft();
    cfg.enabled = true;
    const bool ok = app_alarm_save_config(cfg);
    reload_draft_from_saved();
    return ok;
}

static const char* value_rtc_time()
{
    static char buf[24];
    Pcf85063DateTime now{};
    if (!pcf85063_read_time(&now) || !now.valid || now.oscillator_stopped) {
        snprintf(buf, sizeof(buf), "待校准");
        return buf;
    }

    snprintf(buf,
             sizeof(buf),
             "%02u-%02u %02u:%02u:%02u",
             (unsigned)now.month,
             (unsigned)now.day,
             (unsigned)now.hour,
             (unsigned)now.minute,
             (unsigned)now.second);
    return buf;
}

static const char* value_alarm_saved_switch()
{
    return app_alarm_get_config().enabled ? "已启用" : "已关闭";
}

static const char* value_alarm_saved_time()
{
    static char buf[12];
    const AppAlarmConfig cfg = app_alarm_get_config();
    snprintf(buf,
             sizeof(buf),
             "%02u:%02u:%02u",
             (unsigned)cfg.hour,
             (unsigned)cfg.minute,
             (unsigned)cfg.second);
    return buf;
}

static const char* value_alarm_switch()
{
    load_draft_if_needed();
    if (s_draft_dirty) {
        return s_draft.enabled ? "已启用*" : "已关闭*";
    }
    return s_draft.enabled ? "已启用" : "已关闭";
}

static bool action_toggle_alarm_switch()
{
    load_draft_if_needed();
    if (s_draft.enabled) {
        const bool ok = app_alarm_disable();
        reload_draft_from_saved();
        return ok;
    }

    s_draft.enabled = true;
    return save_draft_enabled();
}

static const char* value_next_trigger()
{
    static char buf[48];
    if (!app_alarm_next_trigger_text(buf, sizeof(buf))) {
        return buf;
    }
    return buf;
}

static const char* value_alarm_time()
{
    static char buf[12];
    AppAlarmConfig& cfg = draft();
    snprintf(buf,
             sizeof(buf),
             "%02u:%02u:%02u",
             (unsigned)cfg.hour,
             (unsigned)cfg.minute,
             (unsigned)cfg.second);
    return buf;
}

static const char* value_repeat_mode()
{
    return app_alarm_repeat_label(draft().repeat_mode);
}

static AppAlarmRepeatMode next_repeat_mode(AppAlarmRepeatMode mode)
{
    switch (mode) {
        case AppAlarmRepeatMode::ONCE:     return AppAlarmRepeatMode::DAILY;
        case AppAlarmRepeatMode::DAILY:    return AppAlarmRepeatMode::WEEKDAYS;
        case AppAlarmRepeatMode::WEEKDAYS: return AppAlarmRepeatMode::WEEKENDS;
        case AppAlarmRepeatMode::WEEKENDS: return AppAlarmRepeatMode::WEEKLY;
        case AppAlarmRepeatMode::WEEKLY:   return AppAlarmRepeatMode::ONCE;
        default:                           return AppAlarmRepeatMode::DAILY;
    }
}

static bool action_cycle_repeat_mode()
{
    AppAlarmConfig& cfg = draft();
    cfg.repeat_mode = next_repeat_mode(cfg.repeat_mode);
    if (cfg.repeat_mode == AppAlarmRepeatMode::WEEKLY && (cfg.weekday_mask & APP_ALARM_WEEKDAY_ALL) == 0) {
        cfg.weekday_mask = APP_ALARM_WEEKDAY_WORKDAYS;
    }
    mark_draft_dirty();
    return true;
}

static const char* value_weekday_text()
{
    static char buf[32];
    AppAlarmConfig& cfg = draft();
    if (cfg.repeat_mode != AppAlarmRepeatMode::WEEKLY) {
        snprintf(buf, sizeof(buf), "%s", app_alarm_repeat_label(cfg.repeat_mode));
        return buf;
    }
    if (!app_alarm_weekday_mask_to_text(cfg.weekday_mask, buf, sizeof(buf))) {
        snprintf(buf, sizeof(buf), "未选择");
    }
    return buf;
}

static const char* value_action()
{
    return app_alarm_action_label(draft().action);
}

static bool action_cycle_action()
{
    AppAlarmConfig& cfg = draft();
    cfg.action = (cfg.action == AppAlarmAction::RESUME_LAST)
        ? AppAlarmAction::WAKE_ONLY
        : AppAlarmAction::RESUME_LAST;
    mark_draft_dirty();
    return true;
}

static const char* value_volume()
{
    static char buf[8];
    snprintf(buf, sizeof(buf), "%u", (unsigned)draft().volume);
    return buf;
}

static bool action_cycle_volume()
{
    AppAlarmConfig& cfg = draft();
    uint8_t next = static_cast<uint8_t>(((cfg.volume / 5) + 1) * 5);
    if (next > 100) {
        next = 5;
    }
    cfg.volume = next;
    mark_draft_dirty();
    return true;
}

static bool action_save_enable()
{
    return save_draft_enabled();
}

static bool action_disable_alarm()
{
    const bool ok = app_alarm_disable();
    reload_draft_from_saved();
    return ok;
}

static bool action_delete_alarm()
{
    const bool ok = app_alarm_delete();
    reload_draft_from_saved();
    return ok;
}

static const char* value_hour()
{
    static char buf[4];
    snprintf(buf, sizeof(buf), "%02u", (unsigned)draft().hour);
    return buf;
}

static bool action_cycle_hour()
{
    AppAlarmConfig& cfg = draft();
    cfg.hour = static_cast<uint8_t>((cfg.hour + 1) % 24);
    mark_draft_dirty();
    return true;
}

static const char* value_minute()
{
    static char buf[4];
    snprintf(buf, sizeof(buf), "%02u", (unsigned)draft().minute);
    return buf;
}

static bool action_cycle_minute()
{
    AppAlarmConfig& cfg = draft();
    cfg.minute = static_cast<uint8_t>((cfg.minute + 1) % 60);
    mark_draft_dirty();
    return true;
}

static const char* value_second()
{
    static char buf[4];
    snprintf(buf, sizeof(buf), "%02u", (unsigned)draft().second);
    return buf;
}

static bool action_cycle_second()
{
    AppAlarmConfig& cfg = draft();
    cfg.second = static_cast<uint8_t>((cfg.second + 1) % 60);
    mark_draft_dirty();
    return true;
}

static const char* weekday_onoff(uint8_t bit)
{
    return (draft().weekday_mask & bit) ? "开" : "关";
}

static bool toggle_weekday(uint8_t bit)
{
    AppAlarmConfig& cfg = draft();
    uint8_t next = cfg.weekday_mask ^ bit;
    next &= APP_ALARM_WEEKDAY_ALL;
    if (next == 0) {
        // 每周指定至少保留一天，避免保存时配置无效。
        return false;
    }
    cfg.weekday_mask = next;
    cfg.repeat_mode = AppAlarmRepeatMode::WEEKLY;
    mark_draft_dirty();
    return true;
}

static const char* value_mon() { return weekday_onoff(APP_ALARM_WEEKDAY_MON); }
static const char* value_tue() { return weekday_onoff(APP_ALARM_WEEKDAY_TUE); }
static const char* value_wed() { return weekday_onoff(APP_ALARM_WEEKDAY_WED); }
static const char* value_thu() { return weekday_onoff(APP_ALARM_WEEKDAY_THU); }
static const char* value_fri() { return weekday_onoff(APP_ALARM_WEEKDAY_FRI); }
static const char* value_sat() { return weekday_onoff(APP_ALARM_WEEKDAY_SAT); }
static const char* value_sun() { return weekday_onoff(APP_ALARM_WEEKDAY_SUN); }

static bool action_toggle_mon() { return toggle_weekday(APP_ALARM_WEEKDAY_MON); }
static bool action_toggle_tue() { return toggle_weekday(APP_ALARM_WEEKDAY_TUE); }
static bool action_toggle_wed() { return toggle_weekday(APP_ALARM_WEEKDAY_WED); }
static bool action_toggle_thu() { return toggle_weekday(APP_ALARM_WEEKDAY_THU); }
static bool action_toggle_fri() { return toggle_weekday(APP_ALARM_WEEKDAY_FRI); }
static bool action_toggle_sat() { return toggle_weekday(APP_ALARM_WEEKDAY_SAT); }
static bool action_toggle_sun() { return toggle_weekday(APP_ALARM_WEEKDAY_SUN); }

const QuickMenuItem TIME_ALARM_ITEMS[] = {
    {"当前时间", QuickMenuItemType::Status, QuickMenuPage::TimeAlarm, "", value_rtc_time, nullptr, true, false},
    {"闹钟状态", QuickMenuItemType::Status, QuickMenuPage::TimeAlarm, "", value_alarm_saved_switch, nullptr, true, false},
    {"闹钟时间", QuickMenuItemType::Status, QuickMenuPage::TimeAlarm, "", value_alarm_saved_time, nullptr, true, false},
    {"下次触发", QuickMenuItemType::Status, QuickMenuPage::TimeAlarm, "", value_next_trigger, nullptr, true, false},
    {"闹钟设置", QuickMenuItemType::SubPage, QuickMenuPage::AlarmSettings, "", value_alarm_saved_switch, nullptr, true, false},
    {"返回", QuickMenuItemType::Back, QuickMenuPage::Root, "", nullptr, nullptr, true, false},
};

const QuickMenuItem ALARM_SETTINGS_ITEMS[] = {
    {"闹钟开关", QuickMenuItemType::Toggle, QuickMenuPage::AlarmSettings, "", value_alarm_switch, action_toggle_alarm_switch, true, false},
    {"闹钟时间", QuickMenuItemType::SubPage, QuickMenuPage::AlarmTime, "", value_alarm_time, nullptr, true, false},
    {"重复模式", QuickMenuItemType::Toggle, QuickMenuPage::AlarmSettings, "", value_repeat_mode, action_cycle_repeat_mode, true, false},
    {"每周选择", QuickMenuItemType::SubPage, QuickMenuPage::AlarmWeekday, "", value_weekday_text, nullptr, true, false},
    {"响铃动作", QuickMenuItemType::Toggle, QuickMenuPage::AlarmSettings, "", value_action, action_cycle_action, true, false},
    {"闹钟音量", QuickMenuItemType::Toggle, QuickMenuPage::AlarmSettings, "", value_volume, action_cycle_volume, true, false},
    {"保存启用", QuickMenuItemType::Action, QuickMenuPage::AlarmSettings, "", nullptr, action_save_enable, true, false},
    {"关闭闹钟", QuickMenuItemType::Action, QuickMenuPage::AlarmSettings, "", nullptr, action_disable_alarm, true, false},
    {"删除闹钟", QuickMenuItemType::Action, QuickMenuPage::AlarmSettings, "", nullptr, action_delete_alarm, true, false},
    {"返回", QuickMenuItemType::Back, QuickMenuPage::TimeAlarm, "", nullptr, nullptr, true, false},
};

const QuickMenuItem ALARM_TIME_ITEMS[] = {
    {"小时", QuickMenuItemType::Toggle, QuickMenuPage::AlarmTime, "", value_hour, action_cycle_hour, true, false},
    {"分钟", QuickMenuItemType::Toggle, QuickMenuPage::AlarmTime, "", value_minute, action_cycle_minute, true, false},
    {"秒", QuickMenuItemType::Toggle, QuickMenuPage::AlarmTime, "", value_second, action_cycle_second, true, false},
    {"保存启用", QuickMenuItemType::Action, QuickMenuPage::AlarmTime, "", nullptr, action_save_enable, true, false},
    {"返回", QuickMenuItemType::Back, QuickMenuPage::AlarmSettings, "", nullptr, nullptr, true, false},
};

const QuickMenuItem ALARM_WEEKDAY_ITEMS[] = {
    {"周一", QuickMenuItemType::Toggle, QuickMenuPage::AlarmWeekday, "", value_mon, action_toggle_mon, true, false},
    {"周二", QuickMenuItemType::Toggle, QuickMenuPage::AlarmWeekday, "", value_tue, action_toggle_tue, true, false},
    {"周三", QuickMenuItemType::Toggle, QuickMenuPage::AlarmWeekday, "", value_wed, action_toggle_wed, true, false},
    {"周四", QuickMenuItemType::Toggle, QuickMenuPage::AlarmWeekday, "", value_thu, action_toggle_thu, true, false},
    {"周五", QuickMenuItemType::Toggle, QuickMenuPage::AlarmWeekday, "", value_fri, action_toggle_fri, true, false},
    {"周六", QuickMenuItemType::Toggle, QuickMenuPage::AlarmWeekday, "", value_sat, action_toggle_sat, true, false},
    {"周日", QuickMenuItemType::Toggle, QuickMenuPage::AlarmWeekday, "", value_sun, action_toggle_sun, true, false},
    {"保存启用", QuickMenuItemType::Action, QuickMenuPage::AlarmWeekday, "", nullptr, action_save_enable, true, false},
    {"返回", QuickMenuItemType::Back, QuickMenuPage::AlarmSettings, "", nullptr, nullptr, true, false},
};

} // namespace

void quick_menu_reset_alarm_draft()
{
    s_draft = AppAlarmConfig{};
    s_draft_loaded = false;
    s_draft_dirty = false;
}

const QuickMenuPageDef& quick_menu_get_time_alarm_page()
{
    static const QuickMenuPageDef page = {
        "时间与闹钟",
        QuickMenuPage::TimeAlarm,
        QuickMenuPage::Root,
        TIME_ALARM_ITEMS,
        MENU_COUNT(TIME_ALARM_ITEMS),
    };
    return page;
}

const QuickMenuPageDef& quick_menu_get_alarm_settings_page()
{
    static const QuickMenuPageDef page = {
        "闹钟设置",
        QuickMenuPage::AlarmSettings,
        QuickMenuPage::TimeAlarm,
        ALARM_SETTINGS_ITEMS,
        MENU_COUNT(ALARM_SETTINGS_ITEMS),
    };
    return page;
}

const QuickMenuPageDef& quick_menu_get_alarm_time_page()
{
    static const QuickMenuPageDef page = {
        "闹钟时间",
        QuickMenuPage::AlarmTime,
        QuickMenuPage::AlarmSettings,
        ALARM_TIME_ITEMS,
        MENU_COUNT(ALARM_TIME_ITEMS),
    };
    return page;
}

const QuickMenuPageDef& quick_menu_get_alarm_weekday_page()
{
    static const QuickMenuPageDef page = {
        "每周选择",
        QuickMenuPage::AlarmWeekday,
        QuickMenuPage::AlarmSettings,
        ALARM_WEEKDAY_ITEMS,
        MENU_COUNT(ALARM_WEEKDAY_ITEMS),
    };
    return page;
}
