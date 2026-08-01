#include "hal/hall_control.h"

#include <Arduino.h>

#include "app_flags.h"
#include "app_state.h"
#include "board/board_pins.h"
#include "hal/board_hw_control.h"
#include "player_control.h"
#include "ui/ui.h"
#include "utils/log.h"
#include "web/web_settings.h"

namespace {

// 当前硬件为低电平表示摆臂磁铁靠近霍尔。
constexpr int HALL_ACTIVE_LEVEL = LOW;
// 机械摆臂到位后只需短消抖，避免过长消抖吞掉电磁铁有效脉冲时间。
constexpr uint32_t HALL_DEBOUNCE_MS = 30;
// 线圈全功率驱动时间由 150ms 提高到 220ms。
constexpr uint32_t SOLENOID_DRIVE_MS = 220;
// 线圈断电后再留少量机械稳定时间；总窗口仍低于 300ms 硬保护上限。
constexpr uint32_t SOLENOID_TARGET_TIMEOUT_MS = 260;

enum class HallMotionTarget : uint8_t {
    None = 0,
    Near,
    Far,
};

bool s_inited = false;
bool s_last_hall_enabled = true;
bool s_last_solenoid_enabled = true;
int s_last_raw = HIGH;
int s_stable_level = HIGH;
uint32_t s_raw_change_ms = 0;
bool s_pause_latched = false;
HallMotionTarget s_motion_target = HallMotionTarget::None;
uint32_t s_motion_started_ms = 0;
// 仅用于 NFC 等“到播放位后再执行”的延迟动作。普通播放键联动保持为空。
HallPlayPositionCallback s_motion_callback = nullptr;
// 动作超时后忽略迟到的目标边沿，保证失败路径不改变播放状态。
bool s_suppress_late_target_edge = false;
int s_suppressed_target_level = HIGH;

bool hall_actions_allowed()
{
    return g_app_state == STATE_PLAYER && !app_rescan_state_get().rescanning;
}

bool effective_hall_enabled(const WebRuntimeSettings& settings)
{
    // 防御旧 NVS 数据：即使历史配置出现“电磁铁开、霍尔关”，运行期仍强制联动。
    return settings.hall_control_enabled || settings.solenoid_enabled;
}

void sync_input_level()
{
    const int level = digitalRead(PIN_HALL_OUT);
    const uint32_t now = millis();

    s_last_raw = level;
    s_stable_level = level;
    s_raw_change_ms = now;
}

const char* motion_target_label(HallMotionTarget target)
{
    switch (target) {
        case HallMotionTarget::Near: return "靠近";
        case HallMotionTarget::Far:  return "离开";
        case HallMotionTarget::None:
        default:                     return "无";
    }
}

bool motion_target_reached()
{
    if (s_motion_target == HallMotionTarget::Near) {
        return s_stable_level == HALL_ACTIVE_LEVEL;
    }
    if (s_motion_target == HallMotionTarget::Far) {
        return s_stable_level != HALL_ACTIVE_LEVEL;
    }
    return false;
}

void clear_motion_state(bool stop_output)
{
    if (stop_output) {
        (void)board_hw_solenoid_stop();
    }
    s_motion_target = HallMotionTarget::None;
    s_motion_started_ms = 0;
    s_motion_callback = nullptr;
}

void cancel_motion(bool stop_output, const char* reason)
{
    const HallMotionTarget target = s_motion_target;
    const HallPlayPositionCallback callback = s_motion_callback;
    const bool had_motion = target != HallMotionTarget::None;

    clear_motion_state(stop_output);

    if (had_motion && callback) {
        LOGW("[HALL] 延迟起播动作已取消：目标=%s 原因=%s",
             motion_target_label(target),
             reason ? reason : "状态变化");
        callback(false);
    }
}

bool apply_hall_playback_state(bool near, const char* reason)
{
    if (!hall_actions_allowed()) {
        LOGD("[HALL] 当前状态不允许控制播放：目标=%s 原因=%s",
             near ? "暂停" : "播放",
             reason ? reason : "unknown");
        return false;
    }

    const PlayerPlaybackState state = player_playback_state_get();
    if (near && state == PlayerPlaybackState::Stopped) {
        // 停止态已经满足“非播放”目标，不应把它当成联动失败。
        s_pause_latched = true;
        LOGI("[HALL] 磁铁靠近，播放器已停止，无需重复暂停：原因=%s",
             reason ? reason : "状态变化");
        return true;
    }

    const bool ok = player_set_paused(near, PlayerToggleTrigger::Hall);
    if (ok) {
        s_pause_latched = near;
        LOGI("[HALL] %s，已%s播放：原因=%s",
             near ? "磁铁靠近" : "磁铁离开",
             near ? "暂停" : "恢复",
             reason ? reason : "状态变化");
        return true;
    }

    LOGW("[HALL] 到位后播放控制失败：位置=%s 原因=%s",
         near ? "靠近" : "离开",
         reason ? reason : "状态变化");
    return false;
}

void finish_motion_at_target()
{
    const bool near = hall_control_is_near();
    const HallMotionTarget reached = s_motion_target;
    const HallPlayPositionCallback callback = s_motion_callback;

    clear_motion_state(true);
    LOGI("[HALL] 摆臂已到位：目标=%s 实际=%s",
         motion_target_label(reached),
         near ? "靠近" : "离开");

    if (callback) {
        // NFC 延迟起播只允许“离开霍尔”的播放位成功。
        const bool success = reached == HallMotionTarget::Far && !near;
        LOGI("[HALL] 延迟起播到位回调：成功=%d", success ? 1 : 0);
        callback(success);
        return;
    }

    if (!apply_hall_playback_state(near, "电磁铁到位")) {
        ui_show_notice_popup("摆臂已到位", "播放状态切换失败");
    }
}

void fail_motion_timeout()
{
    const HallMotionTarget target = s_motion_target;
    const HallPlayPositionCallback callback = s_motion_callback;
    s_suppress_late_target_edge = true;
    s_suppressed_target_level = target == HallMotionTarget::Near
        ? HALL_ACTIVE_LEVEL
        : (HALL_ACTIVE_LEVEL == LOW ? HIGH : LOW);
    clear_motion_state(true);

    LOGE("[HALL] 摆臂到位超时：目标=%s 当前=%s 超时=%lums",
         motion_target_label(target),
         hall_control_is_near() ? "靠近" : "离开",
         static_cast<unsigned long>(SOLENOID_TARGET_TIMEOUT_MS));
    ui_show_notice_popup("摆臂未到位", "请检查电磁铁或机械结构");

    if (callback) {
        callback(false);
    }
}

} // namespace

void hall_control_begin()
{
    pinMode(PIN_HALL_OUT, INPUT_PULLUP);
    sync_input_level();
    s_pause_latched = false;
    s_motion_target = HallMotionTarget::None;
    s_motion_started_ms = 0;
    s_motion_callback = nullptr;
    s_suppress_late_target_edge = false;
    s_suppressed_target_level = HIGH;

    const WebRuntimeSettings settings = web_settings_get();
    s_last_hall_enabled = effective_hall_enabled(settings);
    s_last_solenoid_enabled = settings.solenoid_enabled;
    s_inited = true;

    LOGI("[HALL] 初始化 GPIO%d，有效电平=%d，当前=%s HALL=%d SOL=%d",
         PIN_HALL_OUT,
         HALL_ACTIVE_LEVEL,
         hall_control_is_near() ? "靠近" : "离开",
         s_last_hall_enabled ? 1 : 0,
         s_last_solenoid_enabled ? 1 : 0);
}

void hall_control_tick()
{
    if (!s_inited) {
        hall_control_begin();
    }

    const uint32_t now = millis();
    const int raw = digitalRead(PIN_HALL_OUT);

    if (raw != s_last_raw) {
        s_last_raw = raw;
        s_raw_change_ms = now;
    }

    bool stable_changed = false;
    if (raw != s_stable_level &&
        static_cast<uint32_t>(now - s_raw_change_ms) >= HALL_DEBOUNCE_MS) {
        s_stable_level = raw;
        stable_changed = true;
        LOGI("[HALL] 稳定状态=%s", hall_control_is_near() ? "靠近" : "离开");
    }

    const WebRuntimeSettings settings = web_settings_get();
    const bool hall_enabled = effective_hall_enabled(settings);
    const bool solenoid_enabled = settings.solenoid_enabled;

    if (!solenoid_enabled && s_last_solenoid_enabled) {
        // 菜单或网页关闭电磁铁时立即取消未完成动作，霍尔仍可独立工作。
        cancel_motion(true, "电磁铁联动关闭");
        LOGI("[HALL] 电磁铁联动已关闭，霍尔独立控制=%d", hall_enabled ? 1 : 0);
    } else if (solenoid_enabled && !s_last_solenoid_enabled) {
        LOGI("[HALL] 电磁铁联动已开启，霍尔已强制开启，当前=%s",
             hall_control_is_near() ? "靠近" : "离开");
    }
    s_last_solenoid_enabled = solenoid_enabled;

    if (!hall_enabled) {
        if (s_last_hall_enabled) {
            cancel_motion(true, "霍尔控制关闭");
            s_pause_latched = false;
            s_suppress_late_target_edge = false;
            LOGI("[HALL] 控制已关闭，保持当前播放状态");
        }
        s_last_hall_enabled = false;
        return;
    }

    if (!s_last_hall_enabled) {
        s_last_hall_enabled = true;
        s_pause_latched = false;
        s_suppress_late_target_edge = false;
        LOGI("[HALL] 控制已开启，当前=%s；首次不自动改变播放状态",
             hall_control_is_near() ? "靠近" : "离开");
    }

    if (s_motion_target != HallMotionTarget::None) {
        if (stable_changed && motion_target_reached()) {
            finish_motion_at_target();
            return;
        }

        if (static_cast<uint32_t>(now - s_motion_started_ms) >=
            SOLENOID_TARGET_TIMEOUT_MS) {
            fail_motion_timeout();
        }
        // 电磁铁动作期间忽略非目标抖动，不让瞬态霍尔电平误改播放状态。
        return;
    }

    // 没有电磁铁动作时，只响应稳定边沿一次，不用持续电平覆盖实体播放键。
    if (stable_changed) {
        if (s_suppress_late_target_edge && s_stable_level == s_suppressed_target_level) {
            s_suppress_late_target_edge = false;
            LOGW("[HALL] 已忽略动作超时后迟到的目标边沿：状态=%s",
                 hall_control_is_near() ? "靠近" : "离开");
            return;
        }
        s_suppress_late_target_edge = false;
        (void)apply_hall_playback_state(hall_control_is_near(),
                                        solenoid_enabled ? "摆臂手动移动" : "霍尔独立边沿");
    }
}

bool hall_control_handle_user_toggle()
{
    const WebRuntimeSettings settings = web_settings_get();
    if (!settings.solenoid_enabled) {
        return false;
    }

    if (!s_inited) {
        hall_control_begin();
    }

    // 电磁铁开启时，本次实体按键或 Web 请求始终由联动状态机接管；
    // 目标位置由播放器状态决定，而不是简单把摆臂翻到另一端：
    // - 正在播放：目标为靠近霍尔的停止位；
    // - 暂停或停止：目标为离开霍尔的播放位。
    if (!hall_actions_allowed()) {
        LOGW("[HALL] 当前状态不允许驱动摆臂");
        ui_show_notice_popup("无法驱动摆臂", "请退出扫描或返回播放器");
        return true;
    }

    if (s_motion_target != HallMotionTarget::None || board_hw_solenoid_is_busy()) {
        LOGD("[HALL] 摆臂动作尚未结束，忽略重复播放/暂停请求");
        ui_show_notice_popup("摆臂动作中", "请稍候");
        return true;
    }

    s_suppress_late_target_edge = false;
    const PlayerPlaybackState playback_state = player_playback_state_get();
    const bool request_pause = playback_state == PlayerPlaybackState::Playing;
    const bool currently_near = hall_control_is_near();
    const HallMotionTarget target = request_pause
        ? HallMotionTarget::Near
        : HallMotionTarget::Far;
    const bool already_at_target = target == HallMotionTarget::Near
        ? currently_near
        : !currently_near;

    if (already_at_target) {
        const char* state_label = playback_state == PlayerPlaybackState::Playing
            ? "播放中"
            : (playback_state == PlayerPlaybackState::Paused ? "已暂停" : "已停止");
        LOGI("[HALL] 摆臂已在请求目标位：播放器=%s 当前=%s 目标=%s，直接%s",
             state_label,
             currently_near ? "靠近" : "离开",
             motion_target_label(target),
             request_pause ? "暂停" : "开始/恢复播放");

        if (!apply_hall_playback_state(currently_near, "用户请求且摆臂已到位")) {
            ui_show_notice_popup("播放状态切换失败", "请检查当前歌曲或播放源");
        }
        return true;
    }

    const bool started = target == HallMotionTarget::Near
        ? board_hw_solenoid_move_hall_near(SOLENOID_DRIVE_MS)
        : board_hw_solenoid_move_hall_far(SOLENOID_DRIVE_MS);

    if (!started) {
        LOGE("[HALL] 电磁铁启动失败：目标=%s", motion_target_label(target));
        ui_show_notice_popup("电磁铁启动失败", "请检查驱动芯片和I2C");
        return true;
    }

    s_motion_target = target;
    s_motion_started_ms = millis();
    s_motion_callback = nullptr;
    LOGI("[HALL] 播放/暂停请求驱动摆臂：播放器=%s 当前=%s 目标=%s 脉冲=%lums",
         playback_state == PlayerPlaybackState::Playing
             ? "播放中"
             : (playback_state == PlayerPlaybackState::Paused ? "已暂停" : "已停止"),
         currently_near ? "靠近" : "离开",
         motion_target_label(target),
         static_cast<unsigned long>(SOLENOID_DRIVE_MS));
    return true;
}

HallPlayPositionRequestResult hall_control_request_play_position(
    HallPlayPositionCallback callback)
{
    const WebRuntimeSettings settings = web_settings_get();
    if (!settings.solenoid_enabled) {
        return HallPlayPositionRequestResult::Ready;
    }

    if (!s_inited) {
        hall_control_begin();
    }

    if (!callback) {
        LOGE("[HALL] 延迟起播请求缺少完成回调");
        return HallPlayPositionRequestResult::Failed;
    }

    if (!hall_actions_allowed()) {
        LOGW("[HALL] 当前状态不允许执行 NFC 起播摆臂动作");
        ui_show_notice_popup("无法驱动摆臂", "请退出扫描或返回播放器");
        return HallPlayPositionRequestResult::Failed;
    }

    if (s_motion_target != HallMotionTarget::None || board_hw_solenoid_is_busy()) {
        LOGW("[HALL] 已有摆臂动作，拒绝新的 NFC 起播请求");
        ui_show_notice_popup("摆臂动作中", "本次NFC请求未执行");
        return HallPlayPositionRequestResult::Failed;
    }

    if (!hall_control_is_near()) {
        LOGI("[HALL] 摆臂已在播放位，NFC可立即起播");
        return HallPlayPositionRequestResult::Ready;
    }

    s_suppress_late_target_edge = false;
    if (!board_hw_solenoid_move_hall_far(SOLENOID_DRIVE_MS)) {
        LOGE("[HALL] NFC起播驱动失败：目标=离开");
        ui_show_notice_popup("电磁铁启动失败", "NFC歌曲未播放");
        return HallPlayPositionRequestResult::Failed;
    }

    s_motion_target = HallMotionTarget::Far;
    s_motion_started_ms = millis();
    s_motion_callback = callback;
    LOGI("[HALL] NFC起播驱动摆臂：当前=靠近 目标=离开 脉冲=%lums",
         static_cast<unsigned long>(SOLENOID_DRIVE_MS));
    return HallPlayPositionRequestResult::Started;
}

bool hall_control_is_near()
{
    return s_stable_level == HALL_ACTIVE_LEVEL;
}

bool hall_control_motion_active()
{
    return s_motion_target != HallMotionTarget::None;
}

bool hall_control_blocks_resume()
{
    const WebRuntimeSettings settings = web_settings_get();
    // 只有电磁铁联动模式把摆臂位置视为强约束。
    // 电磁铁关闭、霍尔独立开启时，实体播放键仍可直接恢复，直到霍尔再次发生边沿。
    return s_inited &&
           settings.solenoid_enabled &&
           effective_hall_enabled(settings) &&
           hall_control_is_near();
}

bool hall_control_pause_latched()
{
    return s_pause_latched;
}
