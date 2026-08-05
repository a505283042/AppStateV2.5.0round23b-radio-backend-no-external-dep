#include "app_alarm.h"

#include <Preferences.h>
#include <string.h>

#include "hal/pcf85063.h"
#include "utils/log.h"

namespace {

static const char* kPrefsNs = "alarm";
static const uint8_t kConfigVersion = 2;

static AppAlarmConfig s_cfg{};
static bool s_loaded = false;
static bool s_last_schedule_ok = false;
static char s_last_schedule_message[64] = "未安排";

static bool s_wakeup_pending = false;
static AppAlarmAction s_wakeup_action = AppAlarmAction::WAKE_ONLY;
static uint8_t s_wakeup_volume = 30;

struct AppAlarmNextTrigger {
    Pcf85063DateTime time{};
    uint8_t days_from_now = 0;
    bool valid = false;
};

static const char* weekday_label(uint8_t weekday)
{
    switch (weekday) {
        case 0: return "周日";
        case 1: return "周一";
        case 2: return "周二";
        case 3: return "周三";
        case 4: return "周四";
        case 5: return "周五";
        case 6: return "周六";
        default: return "未知";
    }
}

static void set_schedule_message(const char* msg)
{
    if (!msg) msg = "未知";
    snprintf(s_last_schedule_message, sizeof(s_last_schedule_message), "%s", msg);
}

static bool is_leap_year(uint16_t year)
{
    return ((year % 4) == 0 && (year % 100) != 0) || ((year % 400) == 0);
}

static uint8_t days_in_month(uint16_t year, uint8_t month)
{
    switch (month) {
        case 1:
        case 3:
        case 5:
        case 7:
        case 8:
        case 10:
        case 12:
            return 31;
        case 4:
        case 6:
        case 9:
        case 11:
            return 30;
        case 2:
            return is_leap_year(year) ? 29 : 28;
        default:
            return 0;
    }
}

static bool advance_one_day(Pcf85063DateTime& t)
{
    const uint8_t dim = days_in_month(t.year, t.month);
    if (dim == 0) return false;

    t.second = 0;
    t.minute = 0;
    t.hour = 0;

    if (t.day < dim) {
        ++t.day;
    } else {
        t.day = 1;
        if (t.month < 12) {
            ++t.month;
        } else {
            t.month = 1;
            ++t.year;
            if (t.year > 2099) return false;
        }
    }

    t.weekday = static_cast<uint8_t>((t.weekday + 1) % 7);
    t.valid = true;
    t.oscillator_stopped = false;
    return true;
}

static bool add_days(Pcf85063DateTime& t, uint8_t days)
{
    for (uint8_t i = 0; i < days; ++i) {
        if (!advance_one_day(t)) return false;
    }
    return true;
}

static uint32_t seconds_of_day(uint8_t hour, uint8_t minute, uint8_t second)
{
    return (uint32_t)hour * 3600UL + (uint32_t)minute * 60UL + second;
}

static bool weekday_allowed(uint8_t mask, uint8_t weekday)
{
    if (weekday > 6) return false;
    return (mask & (1u << weekday)) != 0;
}

static bool rtc_read_valid_time(Pcf85063DateTime& out)
{
    return pcf85063_read_time(&out) && out.valid && !out.oscillator_stopped;
}

static AppAlarmConfig normalize_config(AppAlarmConfig cfg)
{
    cfg.weekday_mask &= APP_ALARM_WEEKDAY_ALL;
    switch (cfg.repeat_mode) {
        case AppAlarmRepeatMode::DAILY:
            cfg.weekday_mask = APP_ALARM_WEEKDAY_ALL;
            break;
        case AppAlarmRepeatMode::WEEKDAYS:
            cfg.weekday_mask = APP_ALARM_WEEKDAY_WORKDAYS;
            break;
        case AppAlarmRepeatMode::WEEKENDS:
            cfg.weekday_mask = APP_ALARM_WEEKDAY_WEEKENDS;
            break;
        case AppAlarmRepeatMode::ONCE:
            // 单次闹钟使用“下一次时刻”，星期掩码不参与计算。
            if (cfg.weekday_mask == 0) cfg.weekday_mask = APP_ALARM_WEEKDAY_ALL;
            break;
        case AppAlarmRepeatMode::WEEKLY:
            // 每周指定星期必须至少选择一天。
            break;
    }
    return cfg;
}

static bool compute_next_trigger(const AppAlarmConfig& raw_cfg,
                                 const Pcf85063DateTime& now,
                                 AppAlarmNextTrigger& out)
{
    out = AppAlarmNextTrigger{};
    if (!now.valid || now.oscillator_stopped || !app_alarm_validate_config(raw_cfg)) {
        return false;
    }

    const AppAlarmConfig cfg = normalize_config(raw_cfg);
    const uint32_t now_sod = seconds_of_day(now.hour, now.minute, now.second);
    const uint32_t alarm_sod = seconds_of_day(cfg.hour, cfg.minute, cfg.second);
    const uint8_t mask = app_alarm_effective_weekday_mask(cfg);

    const uint8_t max_days = (cfg.repeat_mode == AppAlarmRepeatMode::ONCE) ? 1 : 7;
    for (uint8_t offset = 0; offset <= max_days; ++offset) {
        Pcf85063DateTime candidate = now;
        if (!add_days(candidate, offset)) {
            return false;
        }

        if (cfg.repeat_mode != AppAlarmRepeatMode::ONCE && !weekday_allowed(mask, candidate.weekday)) {
            continue;
        }

        // 今天同一时间或已经过去的时间不再安排，避免刚清 AF 后同一秒再次触发。
        if (offset == 0 && alarm_sod <= now_sod) {
            continue;
        }

        candidate.hour = cfg.hour;
        candidate.minute = cfg.minute;
        candidate.second = cfg.second;
        candidate.valid = true;
        candidate.oscillator_stopped = false;
        out.time = candidate;
        out.days_from_now = offset;
        out.valid = true;
        return true;
    }

    return false;
}

static bool save_config_to_nvs(const AppAlarmConfig& raw_cfg)
{
    const AppAlarmConfig cfg = normalize_config(raw_cfg);

    Preferences pref;
    if (!pref.begin(kPrefsNs, false)) {
        LOGE("[闹钟] 保存失败：打开 NVS namespace");
        return false;
    }

    bool ok = true;
    ok &= pref.putUChar("ver", kConfigVersion) > 0;
    ok &= pref.putBool("enabled", cfg.enabled);
    ok &= pref.putUChar("hour", cfg.hour) > 0;
    ok &= pref.putUChar("minute", cfg.minute) > 0;
    ok &= pref.putUChar("second", cfg.second) > 0;
    ok &= pref.putUChar("repeat", static_cast<uint8_t>(cfg.repeat_mode)) > 0;
    ok &= pref.putUChar("wmask", cfg.weekday_mask) > 0;
    ok &= pref.putUChar("action", static_cast<uint8_t>(cfg.action)) > 0;
    ok &= pref.putUChar("volume", cfg.volume) > 0;
    pref.end();

    if (!ok) {
        LOGE("[闹钟] 保存失败：写入 NVS");
        return false;
    }

    LOGI("[闹钟] 配置已保存：启用=%d 时间=%02u:%02u:%02u 重复=%s 星期掩码=0x%02X 动作=%s 音量=%u",
         cfg.enabled ? 1 : 0,
         (unsigned)cfg.hour,
         (unsigned)cfg.minute,
         (unsigned)cfg.second,
         app_alarm_repeat_key(cfg.repeat_mode),
         (unsigned)cfg.weekday_mask,
         app_alarm_action_key(cfg.action),
         (unsigned)cfg.volume);
    return true;
}

static bool load_config_from_nvs(AppAlarmConfig& out)
{
    Preferences pref;
    if (!pref.begin(kPrefsNs, true)) {
        LOGW("[闹钟] 加载跳过：打开 NVS namespace 失败");
        return false;
    }

    const bool has_enabled = pref.isKey("enabled");
    if (!has_enabled) {
        pref.end();
        return false;
    }

    AppAlarmConfig cfg{};
    cfg.enabled = pref.getBool("enabled", false);
    cfg.hour = pref.getUChar("hour", cfg.hour);
    cfg.minute = pref.getUChar("minute", cfg.minute);
    cfg.second = pref.getUChar("second", cfg.second);
    cfg.repeat_mode = static_cast<AppAlarmRepeatMode>(pref.getUChar("repeat", static_cast<uint8_t>(cfg.repeat_mode)));
    cfg.weekday_mask = pref.getUChar("wmask", cfg.weekday_mask);
    cfg.action = static_cast<AppAlarmAction>(pref.getUChar("action", static_cast<uint8_t>(cfg.action)));
    cfg.volume = pref.getUChar("volume", cfg.volume);
    pref.end();

    cfg = normalize_config(cfg);
    if (!app_alarm_validate_config(cfg)) {
        LOGW("[闹钟] NVS 配置无效，使用默认值");
        return false;
    }

    out = cfg;
    return true;
}

static bool clear_config_from_nvs()
{
    Preferences pref;
    if (!pref.begin(kPrefsNs, false)) {
        LOGE("[闹钟] 删除失败：打开 NVS namespace");
        return false;
    }
    const bool ok = pref.clear();
    pref.end();
    if (!ok) {
        LOGE("[闹钟] 删除失败：清空 NVS namespace");
    }
    return ok;
}

static bool schedule_rtc_alarm_for_config(const AppAlarmConfig& raw_cfg)
{
    AppAlarmConfig cfg = normalize_config(raw_cfg);
    s_last_schedule_ok = false;

    if (!cfg.enabled) {
        if (!pcf85063_disable_alarm()) {
            set_schedule_message("RTC闹钟关闭失败");
            LOGW("[闹钟] 关闭 RTC 闹钟失败");
            return false;
        }
        set_schedule_message("已关闭");
        s_last_schedule_ok = true;
        return true;
    }

    if (!pcf85063_is_ready()) {
        set_schedule_message("RTC未就绪");
        LOGW("[闹钟] 安排失败：RTC未就绪");
        return false;
    }

    Pcf85063DateTime now{};
    if (!rtc_read_valid_time(now)) {
        set_schedule_message("请先校准RTC时间");
        LOGW("[闹钟] 安排失败：RTC时间无效");
        return false;
    }

    AppAlarmNextTrigger next{};
    if (!compute_next_trigger(cfg, now, next)) {
        set_schedule_message("无法计算下次触发");
        LOGW("[闹钟] 安排失败：无法计算下次触发");
        return false;
    }

    // 使用“日 + 星期 + 时分秒”安排下一次具体触发，避免单纯每天匹配导致当天重复触发。
    if (!pcf85063_set_alarm(next.time.second,
                            next.time.minute,
                            next.time.hour,
                            next.time.day,
                            next.time.weekday)) {
        set_schedule_message("RTC闹钟写入失败");
        LOGW("[闹钟] 安排失败：写入 RTC 闹钟失败");
        return false;
    }

    s_last_schedule_ok = true;
    char msg[64];
    snprintf(msg,
             sizeof(msg),
             "已安排 %02u-%02u %s %02u:%02u:%02u",
             (unsigned)next.time.month,
             (unsigned)next.time.day,
             weekday_label(next.time.weekday),
             (unsigned)next.time.hour,
             (unsigned)next.time.minute,
             (unsigned)next.time.second);
    set_schedule_message(msg);

    LOGI("[闹钟] RTC 已安排：%04u-%02u-%02u %s %02u:%02u:%02u 重复=%s",
         (unsigned)next.time.year,
         (unsigned)next.time.month,
         (unsigned)next.time.day,
         weekday_label(next.time.weekday),
         (unsigned)next.time.hour,
         (unsigned)next.time.minute,
         (unsigned)next.time.second,
         app_alarm_repeat_key(cfg.repeat_mode));
    return true;
}

static void arm_wakeup_action_if_needed(const AppAlarmConfig& cfg)
{
    if (!pcf85063_boot_alarm_was_pending() || !cfg.enabled) {
        return;
    }

    s_wakeup_pending = true;
    s_wakeup_action = cfg.action;
    s_wakeup_volume = cfg.volume > 100 ? 100 : cfg.volume;
    LOGI("[闹钟] 检测到收音机闹钟开机：动作=%s 音量=%u",
         app_alarm_action_key(s_wakeup_action),
         (unsigned)s_wakeup_volume);
}

static bool disable_once_alarm_after_boot_if_needed(AppAlarmConfig& cfg)
{
    if (!pcf85063_boot_alarm_was_pending() || cfg.repeat_mode != AppAlarmRepeatMode::ONCE || !cfg.enabled) {
        return false;
    }

    cfg.enabled = false;
    if (!save_config_to_nvs(cfg)) {
        set_schedule_message("单次闹钟触发后保存关闭状态失败");
        return true;
    }

    s_last_schedule_ok = true;
    set_schedule_message("单次闹钟已触发并关闭");
    LOGI("[闹钟] 单次闹钟已触发，配置已自动关闭");
    return true;
}

} // namespace

void app_alarm_begin()
{
    if (s_loaded) {
        return;
    }

    s_loaded = true;
    AppAlarmConfig loaded{};
    if (load_config_from_nvs(loaded)) {
        s_cfg = loaded;
        LOGI("[闹钟] 已从 NVS 加载：启用=%d 时间=%02u:%02u:%02u 重复=%s 星期掩码=0x%02X 动作=%s 音量=%u",
             s_cfg.enabled ? 1 : 0,
             (unsigned)s_cfg.hour,
             (unsigned)s_cfg.minute,
             (unsigned)s_cfg.second,
             app_alarm_repeat_key(s_cfg.repeat_mode),
             (unsigned)s_cfg.weekday_mask,
             app_alarm_action_key(s_cfg.action),
             (unsigned)s_cfg.volume);
    } else {
        s_cfg = AppAlarmConfig{};
        LOGI("[闹钟] 使用默认配置");
    }

    if (pcf85063_boot_alarm_was_pending() && s_cfg.enabled) {
        arm_wakeup_action_if_needed(s_cfg);
    }

    if (disable_once_alarm_after_boot_if_needed(s_cfg)) {
        return;
    }

    if (s_cfg.enabled) {
        (void)schedule_rtc_alarm_for_config(s_cfg);
    } else {
        set_schedule_message("未启用");
    }
}

AppAlarmConfig app_alarm_get_config()
{
    if (!s_loaded) {
        app_alarm_begin();
    }
    return s_cfg;
}

bool app_alarm_save_config(const AppAlarmConfig& raw_cfg)
{
    AppAlarmConfig cfg = normalize_config(raw_cfg);
    if (!app_alarm_validate_config(cfg)) {
        LOGW("[闹钟] 保存失败：配置字段无效");
        set_schedule_message("配置字段无效");
        return false;
    }

    // 启用闹钟时，先确认 RTC 能写入；成功后再保存配置，避免页面显示启用但 RTC 未生效。
    if (!schedule_rtc_alarm_for_config(cfg)) {
        return false;
    }

    if (!save_config_to_nvs(cfg)) {
        return false;
    }

    s_cfg = cfg;
    s_loaded = true;
    return true;
}

bool app_alarm_disable()
{
    if (!s_loaded) {
        app_alarm_begin();
    }

    AppAlarmConfig next = s_cfg;
    next.enabled = false;

    if (!schedule_rtc_alarm_for_config(next)) {
        return false;
    }
    if (!save_config_to_nvs(next)) {
        return false;
    }

    s_cfg = next;
    s_loaded = true;
    LOGI("[闹钟] 已关闭，保留配置时间=%02u:%02u:%02u 重复=%s",
         (unsigned)s_cfg.hour,
         (unsigned)s_cfg.minute,
         (unsigned)s_cfg.second,
         app_alarm_repeat_key(s_cfg.repeat_mode));
    return true;
}

bool app_alarm_delete()
{
    // 删除用户闹钟时必须同时关闭 RTC 闹钟，避免配置没了但硬件闹钟还会触发。
    if (!pcf85063_disable_alarm()) {
        set_schedule_message("RTC闹钟关闭失败");
        LOGW("[闹钟] 删除失败：RTC闹钟关闭失败");
        return false;
    }

    if (!clear_config_from_nvs()) {
        return false;
    }

    s_cfg = AppAlarmConfig{};
    s_cfg.enabled = false;
    s_loaded = true;
    s_last_schedule_ok = true;
    set_schedule_message("已删除");
    LOGI("[闹钟] 已删除配置并关闭 RTC 闹钟");
    return true;
}

bool app_alarm_reschedule_after_time_change()
{
    if (!s_loaded) {
        app_alarm_begin();
    }

    if (!s_cfg.enabled) {
        s_last_schedule_ok = true;
        set_schedule_message("时间已校准，闹钟未启用");
        return true;
    }

    const bool ok = schedule_rtc_alarm_for_config(s_cfg);
    LOGI("[闹钟] RTC校时后重新安排：成功=%d 结果=%s",
         ok ? 1 : 0,
         app_alarm_last_schedule_message());
    return ok;
}

bool app_alarm_is_enabled()
{
    return app_alarm_get_config().enabled;
}

bool app_alarm_wakeup_pending()
{
    return s_wakeup_pending;
}

AppAlarmAction app_alarm_wakeup_action()
{
    return s_wakeup_action;
}

uint8_t app_alarm_wakeup_volume()
{
    return s_wakeup_volume > 100 ? 100 : s_wakeup_volume;
}

bool app_alarm_should_block_player_boot_autoplay()
{
    return s_wakeup_pending && s_wakeup_action == AppAlarmAction::WAKE_ONLY;
}

bool app_alarm_should_auto_resume_last()
{
    return s_wakeup_pending && s_wakeup_action == AppAlarmAction::RESUME_LAST;
}

void app_alarm_mark_wakeup_handled(const char* reason)
{
    if (!s_wakeup_pending) {
        return;
    }

    if (!reason || !reason[0]) {
        reason = "已处理";
    }

    LOGI("[闹钟] 闹钟开机动作完成：%s", reason);
    s_wakeup_pending = false;
}

bool app_alarm_validate_config(const AppAlarmConfig& raw_cfg)
{
    AppAlarmConfig cfg = raw_cfg;
    cfg.weekday_mask &= APP_ALARM_WEEKDAY_ALL;

    if (cfg.hour > 23 || cfg.minute > 59 || cfg.second > 59 || cfg.volume > 100) {
        return false;
    }

    switch (cfg.action) {
        case AppAlarmAction::WAKE_ONLY:
        case AppAlarmAction::RESUME_LAST:
            break;
        default:
            return false;
    }

    switch (cfg.repeat_mode) {
        case AppAlarmRepeatMode::ONCE:
        case AppAlarmRepeatMode::DAILY:
        case AppAlarmRepeatMode::WEEKDAYS:
        case AppAlarmRepeatMode::WEEKENDS:
            return true;
        case AppAlarmRepeatMode::WEEKLY:
            return cfg.weekday_mask != 0;
        default:
            return false;
    }
}

const char* app_alarm_action_key(AppAlarmAction action)
{
    switch (action) {
        case AppAlarmAction::WAKE_ONLY:   return "wake_only";
        case AppAlarmAction::RESUME_LAST: return "resume_last";
        default:                          return "unknown";
    }
}

const char* app_alarm_action_label(AppAlarmAction action)
{
    switch (action) {
        case AppAlarmAction::WAKE_ONLY:   return "只开机";
        case AppAlarmAction::RESUME_LAST: return "恢复上次播放";
        default:                          return "未知";
    }
}

bool app_alarm_action_from_key(const String& key, AppAlarmAction& out_action)
{
    String s = key;
    s.trim();
    if (s == "wake_only") {
        out_action = AppAlarmAction::WAKE_ONLY;
        return true;
    }
    if (s == "resume_last") {
        out_action = AppAlarmAction::RESUME_LAST;
        return true;
    }
    return false;
}

const char* app_alarm_repeat_key(AppAlarmRepeatMode mode)
{
    switch (mode) {
        case AppAlarmRepeatMode::ONCE:     return "once";
        case AppAlarmRepeatMode::DAILY:    return "daily";
        case AppAlarmRepeatMode::WEEKDAYS: return "weekdays";
        case AppAlarmRepeatMode::WEEKENDS: return "weekends";
        case AppAlarmRepeatMode::WEEKLY:   return "weekly";
        default:                           return "unknown";
    }
}

const char* app_alarm_repeat_label(AppAlarmRepeatMode mode)
{
    switch (mode) {
        case AppAlarmRepeatMode::ONCE:     return "单次";
        case AppAlarmRepeatMode::DAILY:    return "每天";
        case AppAlarmRepeatMode::WEEKDAYS: return "工作日";
        case AppAlarmRepeatMode::WEEKENDS: return "周末";
        case AppAlarmRepeatMode::WEEKLY:   return "每周指定";
        default:                           return "未知";
    }
}

bool app_alarm_repeat_from_key(const String& key, AppAlarmRepeatMode& out_mode)
{
    String s = key;
    s.trim();
    if (s == "once") {
        out_mode = AppAlarmRepeatMode::ONCE;
        return true;
    }
    if (s == "daily") {
        out_mode = AppAlarmRepeatMode::DAILY;
        return true;
    }
    if (s == "weekdays") {
        out_mode = AppAlarmRepeatMode::WEEKDAYS;
        return true;
    }
    if (s == "weekends") {
        out_mode = AppAlarmRepeatMode::WEEKENDS;
        return true;
    }
    if (s == "weekly") {
        out_mode = AppAlarmRepeatMode::WEEKLY;
        return true;
    }
    return false;
}

uint8_t app_alarm_effective_weekday_mask(const AppAlarmConfig& cfg)
{
    switch (cfg.repeat_mode) {
        case AppAlarmRepeatMode::DAILY:
            return APP_ALARM_WEEKDAY_ALL;
        case AppAlarmRepeatMode::WEEKDAYS:
            return APP_ALARM_WEEKDAY_WORKDAYS;
        case AppAlarmRepeatMode::WEEKENDS:
            return APP_ALARM_WEEKDAY_WEEKENDS;
        case AppAlarmRepeatMode::WEEKLY:
            return cfg.weekday_mask & APP_ALARM_WEEKDAY_ALL;
        case AppAlarmRepeatMode::ONCE:
        default:
            return APP_ALARM_WEEKDAY_ALL;
    }
}

bool app_alarm_weekday_mask_to_text(uint8_t mask, char* out, size_t out_len)
{
    if (!out || out_len == 0) {
        return false;
    }
    out[0] = '\0';

    mask &= APP_ALARM_WEEKDAY_ALL;
    if (mask == 0) {
        snprintf(out, out_len, "未选择");
        return false;
    }
    if (mask == APP_ALARM_WEEKDAY_ALL) {
        snprintf(out, out_len, "每天");
        return true;
    }
    if (mask == APP_ALARM_WEEKDAY_WORKDAYS) {
        snprintf(out, out_len, "周一至周五");
        return true;
    }
    if (mask == APP_ALARM_WEEKDAY_WEEKENDS) {
        snprintf(out, out_len, "周末");
        return true;
    }

    bool first = true;
    for (uint8_t w = 0; w <= 6; ++w) {
        if ((mask & (1u << w)) == 0) continue;
        const char* label = weekday_label(w);
        const size_t used = strlen(out);
        snprintf(out + used, out_len > used ? out_len - used : 0, "%s%s", first ? "" : "、", label);
        first = false;
    }
    return true;
}

bool app_alarm_next_trigger_text(char* out, size_t out_len)
{
    if (!out || out_len == 0) {
        return false;
    }
    out[0] = '\0';

    const AppAlarmConfig cfg = app_alarm_get_config();
    if (!cfg.enabled) {
        snprintf(out, out_len, "未启用");
        return false;
    }

    Pcf85063DateTime now{};
    if (!rtc_read_valid_time(now)) {
        snprintf(out, out_len, "RTC未校准");
        return false;
    }

    AppAlarmNextTrigger next{};
    if (!compute_next_trigger(cfg, now, next)) {
        snprintf(out, out_len, "无法计算");
        return false;
    }

    const char* prefix = nullptr;
    if (next.days_from_now == 0) {
        prefix = "今天";
    } else if (next.days_from_now == 1) {
        prefix = "明天";
    }

    if (prefix) {
        snprintf(out,
                 out_len,
                 "%s %s %02u:%02u:%02u",
                 prefix,
                 weekday_label(next.time.weekday),
                 (unsigned)next.time.hour,
                 (unsigned)next.time.minute,
                 (unsigned)next.time.second);
    } else {
        snprintf(out,
                 out_len,
                 "%02u-%02u %s %02u:%02u:%02u",
                 (unsigned)next.time.month,
                 (unsigned)next.time.day,
                 weekday_label(next.time.weekday),
                 (unsigned)next.time.hour,
                 (unsigned)next.time.minute,
                 (unsigned)next.time.second);
    }
    return true;
}

bool app_alarm_last_schedule_ok()
{
    return s_last_schedule_ok;
}

const char* app_alarm_last_schedule_message()
{
    return s_last_schedule_message;
}
