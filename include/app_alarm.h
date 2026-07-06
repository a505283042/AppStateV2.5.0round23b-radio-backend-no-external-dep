#pragma once

#include <Arduino.h>
#include <stdint.h>

/**
 * @brief 收音机闹钟到点后的动作。
 */
enum class AppAlarmAction : uint8_t {
    WAKE_ONLY = 0,     // 只开机，不自动播放
    RESUME_LAST = 1,   // 后续用于恢复上次播放
};

/**
 * @brief 收音机闹钟重复模式。
 */
enum class AppAlarmRepeatMode : uint8_t {
    ONCE = 0,       // 单次：下一次响铃后关闭配置
    DAILY = 1,      // 每天
    WEEKDAYS = 2,   // 工作日：周一到周五
    WEEKENDS = 3,   // 周末：周六、周日
    WEEKLY = 4,     // 每周指定星期，可选择一个或多个星期
};

/** @brief 星期掩码：bit0=周日，bit1=周一 ... bit6=周六。 */
static constexpr uint8_t APP_ALARM_WEEKDAY_SUN = 1u << 0;
static constexpr uint8_t APP_ALARM_WEEKDAY_MON = 1u << 1;
static constexpr uint8_t APP_ALARM_WEEKDAY_TUE = 1u << 2;
static constexpr uint8_t APP_ALARM_WEEKDAY_WED = 1u << 3;
static constexpr uint8_t APP_ALARM_WEEKDAY_THU = 1u << 4;
static constexpr uint8_t APP_ALARM_WEEKDAY_FRI = 1u << 5;
static constexpr uint8_t APP_ALARM_WEEKDAY_SAT = 1u << 6;
static constexpr uint8_t APP_ALARM_WEEKDAY_ALL = 0x7F;
static constexpr uint8_t APP_ALARM_WEEKDAY_WORKDAYS = APP_ALARM_WEEKDAY_MON |
                                                       APP_ALARM_WEEKDAY_TUE |
                                                       APP_ALARM_WEEKDAY_WED |
                                                       APP_ALARM_WEEKDAY_THU |
                                                       APP_ALARM_WEEKDAY_FRI;
static constexpr uint8_t APP_ALARM_WEEKDAY_WEEKENDS = APP_ALARM_WEEKDAY_SUN |
                                                      APP_ALARM_WEEKDAY_SAT;

/**
 * @brief 收音机闹钟配置。
 */
struct AppAlarmConfig {
    bool enabled = false;
    uint8_t hour = 7;
    uint8_t minute = 30;
    uint8_t second = 0;
    AppAlarmRepeatMode repeat_mode = AppAlarmRepeatMode::DAILY;
    uint8_t weekday_mask = APP_ALARM_WEEKDAY_ALL;
    AppAlarmAction action = AppAlarmAction::RESUME_LAST;
    uint8_t volume = 30;
};

/** @brief 启动时加载闹钟配置，并在需要时安排 RTC 闹钟。 */
void app_alarm_begin();

/** @brief 获取当前闹钟配置快照。 */
AppAlarmConfig app_alarm_get_config();

/** @brief 保存闹钟配置；启用时会立即写入 RTC 闹钟。 */
bool app_alarm_save_config(const AppAlarmConfig& cfg);

/** @brief 关闭闹钟但保留时间、动作、音量和重复模式配置。 */
bool app_alarm_disable();

/** @brief 删除闹钟配置并关闭 RTC 闹钟。 */
bool app_alarm_delete();

/** @brief 当前配置是否启用。 */
bool app_alarm_is_enabled();

/** @brief 本次开机是否由已启用的收音机闹钟触发，并且仍有待处理动作。 */
bool app_alarm_wakeup_pending();

/** @brief 本次闹钟开机动作。 */
AppAlarmAction app_alarm_wakeup_action();

/** @brief 本次闹钟开机使用的目标音量。 */
uint8_t app_alarm_wakeup_volume();

/** @brief 闹钟动作是否要求阻止播放器开机自动播放。 */
bool app_alarm_should_block_player_boot_autoplay();

/** @brief 闹钟动作是否要求恢复上次播放。 */
bool app_alarm_should_auto_resume_last();

/** @brief 播放器完成闹钟开机动作后调用，避免重复执行。 */
void app_alarm_mark_wakeup_handled(const char* reason = nullptr);

/** @brief 当前配置字段是否合法。 */
bool app_alarm_validate_config(const AppAlarmConfig& cfg);

/** @brief 将动作枚举转成机器可读 key。 */
const char* app_alarm_action_key(AppAlarmAction action);

/** @brief 将动作枚举转成中文标签。 */
const char* app_alarm_action_label(AppAlarmAction action);

/** @brief 从 Web/API key 解析动作枚举。 */
bool app_alarm_action_from_key(const String& key, AppAlarmAction& out_action);

/** @brief 将重复模式枚举转成机器可读 key。 */
const char* app_alarm_repeat_key(AppAlarmRepeatMode mode);

/** @brief 将重复模式枚举转成中文标签。 */
const char* app_alarm_repeat_label(AppAlarmRepeatMode mode);

/** @brief 从 Web/API key 解析重复模式枚举。 */
bool app_alarm_repeat_from_key(const String& key, AppAlarmRepeatMode& out_mode);

/** @brief 根据重复模式返回真正参与计算的星期掩码。 */
uint8_t app_alarm_effective_weekday_mask(const AppAlarmConfig& cfg);

/** @brief 将星期掩码转成中文说明。 */
bool app_alarm_weekday_mask_to_text(uint8_t mask, char* out, size_t out_len);

/** @brief 写入“下次触发”文本；闹钟未启用或 RTC 无效时返回 false。 */
bool app_alarm_next_trigger_text(char* out, size_t out_len);

/** @brief 返回最近一次安排 RTC 闹钟是否成功。 */
bool app_alarm_last_schedule_ok();

/** @brief 返回最近一次安排 RTC 闹钟的说明。 */
const char* app_alarm_last_schedule_message();
