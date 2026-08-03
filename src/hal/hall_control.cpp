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
#include "web/web_notice.h"

namespace {

// 当前硬件为低电平表示摆臂磁铁靠近霍尔。
constexpr int HALL_ACTIVE_LEVEL = LOW;
// 霍尔最终确认需要保持目标电平的时间。
constexpr uint32_t HALL_DEBOUNCE_MS = 30;
// 线圈保持完整 220ms 全功率脉冲；脉冲期间不判定机械到位。
constexpr uint32_t SOLENOID_DRIVE_MS = 220;
// 线圈断电后先等待机械惯性和霍尔输出稳定，再开始消抖确认。
constexpr uint32_t SOLENOID_SETTLE_MS = 20;
// 总窗口包含 220ms 驱动和 140ms 断电确认；延长确认时间不会增加线圈发热。
constexpr uint32_t SOLENOID_TARGET_TIMEOUT_MS = 360;

enum class HallMotionTarget : uint8_t {
    None = 0,
    Near,
    Far,
};

enum class HallMotionPhase : uint8_t {
    None = 0,
    Driving,
    Confirming,
    WaitingManual,
};

bool s_inited = false;
bool s_last_hall_enabled = true;
bool s_last_solenoid_enabled = true;
int s_last_raw = HIGH;
int s_stable_level = HIGH;
uint32_t s_raw_change_ms = 0;
bool s_pause_latched = false;
HallMotionTarget s_motion_target = HallMotionTarget::None;
HallMotionPhase s_motion_phase = HallMotionPhase::None;
uint32_t s_motion_started_ms = 0;
uint32_t s_motion_confirm_started_ms = 0;
int s_motion_confirm_raw = HIGH;
uint32_t s_motion_confirm_change_ms = 0;
// 仅用于“到播放位后再执行”的延迟动作。普通播放键联动保持为空。
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

void show_hall_notice(const char* title,
                      const char* detail,
                      WebNoticeLevel level)
{
    ui_show_notice_popup(title, detail);
    web_notice_publish(title, detail, level);
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

bool raw_matches_motion_target(int raw)
{
    if (s_motion_target == HallMotionTarget::Near) {
        return raw == HALL_ACTIVE_LEVEL;
    }
    if (s_motion_target == HallMotionTarget::Far) {
        return raw != HALL_ACTIVE_LEVEL;
    }
    return false;
}

void begin_motion_confirmation(uint32_t now)
{
    s_motion_phase = HallMotionPhase::Confirming;
    s_motion_confirm_started_ms = now;
    s_motion_confirm_raw = digitalRead(PIN_HALL_OUT);
    s_motion_confirm_change_ms = now;

    LOGI("[HALL] 电磁铁脉冲结束，开始霍尔确认：目标=%s 稳定等待=%lums 消抖=%lums",
         motion_target_label(s_motion_target),
         static_cast<unsigned long>(SOLENOID_SETTLE_MS),
         static_cast<unsigned long>(HALL_DEBOUNCE_MS));
}

bool motion_target_confirmed(uint32_t now)
{
    const int raw = digitalRead(PIN_HALL_OUT);
    if (raw != s_motion_confirm_raw) {
        s_motion_confirm_raw = raw;
        s_motion_confirm_change_ms = now;
    }

    if (static_cast<uint32_t>(now - s_motion_confirm_started_ms) <
        SOLENOID_SETTLE_MS) {
        return false;
    }

    if (static_cast<uint32_t>(now - s_motion_confirm_change_ms) <
        HALL_DEBOUNCE_MS) {
        return false;
    }

    if (!raw_matches_motion_target(s_motion_confirm_raw)) {
        return false;
    }

    // 使用脉冲结束后的独立采样结果同步全局稳定状态。
    // 这样即使霍尔在驱动期间已经变化，也不会因为缺少新的 stable_changed 边沿而误判超时。
    s_last_raw = s_motion_confirm_raw;
    s_stable_level = s_motion_confirm_raw;
    s_raw_change_ms = now;
    return true;
}

void clear_motion_state(bool stop_output)
{
    if (stop_output) {
        (void)board_hw_solenoid_stop();
    }
    s_motion_target = HallMotionTarget::None;
    s_motion_phase = HallMotionPhase::None;
    s_motion_started_ms = 0;
    s_motion_confirm_started_ms = 0;
    s_motion_confirm_raw = HIGH;
    s_motion_confirm_change_ms = 0;
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
    const HallMotionPhase phase = s_motion_phase;
    const HallPlayPositionCallback callback = s_motion_callback;

    // 仅霍尔等待模式没有电磁铁输出，不需要额外访问驱动器。
    clear_motion_state(phase != HallMotionPhase::WaitingManual);
    LOGI("[HALL] 摆臂已到位：目标=%s 实际=%s",
         motion_target_label(reached),
         near ? "靠近" : "离开");

    if (callback) {
        // 延迟起播只允许“离开霍尔”的播放位成功。
        const bool success = reached == HallMotionTarget::Far && !near;
        LOGI("[HALL] 延迟起播到位回调：成功=%d", success ? 1 : 0);
        callback(success);
        return;
    }

    if (!apply_hall_playback_state(near, "电磁铁到位")) {
        show_hall_notice("摆臂已到位", "播放状态切换失败", WebNoticeLevel::Error);
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

    LOGE("[HALL] 摆臂到位超时：目标=%s 当前=%s 总窗口=%lums（脉冲=%lums）",
         motion_target_label(target),
         hall_control_is_near() ? "靠近" : "离开",
         static_cast<unsigned long>(SOLENOID_TARGET_TIMEOUT_MS),
         static_cast<unsigned long>(SOLENOID_DRIVE_MS));
    show_hall_notice("摆臂未到位", "请检查电磁铁或机械结构", WebNoticeLevel::Error);

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
    s_motion_phase = HallMotionPhase::None;
    s_motion_started_ms = 0;
    s_motion_confirm_started_ms = 0;
    s_motion_confirm_raw = HIGH;
    s_motion_confirm_change_ms = 0;
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
        // 菜单或网页关闭电磁铁时，只取消仍在输出或确认中的电磁铁动作。
        // 如果关闭后已经提交了“等待用户手动拨动”的新请求，则必须继续保留。
        if (s_motion_phase != HallMotionPhase::WaitingManual) {
            cancel_motion(true, "电磁铁联动关闭");
        }
        LOGI("[HALL] 电磁铁联动已关闭，霍尔独立控制=%d", hall_enabled ? 1 : 0);
    } else if (solenoid_enabled && !s_last_solenoid_enabled) {
        // 仅霍尔模式下暂存的手动到位请求不能在模式切换后继续悬挂。
        if (s_motion_phase == HallMotionPhase::WaitingManual) {
            cancel_motion(false, "电磁铁联动已开启，请重新发起播放请求");
        }
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
        // 仅霍尔模式不设超时：用户可以在提示后手动拨动摆臂。
        // 必须等待全局霍尔状态完成30ms消抖，确认离开后才执行暂存的播放请求。
        if (s_motion_phase == HallMotionPhase::WaitingManual) {
            if (!hall_actions_allowed()) {
                cancel_motion(false, "播放器状态变化");
                return;
            }
            if (stable_changed && raw_matches_motion_target(s_stable_level)) {
                LOGI("[HALL] 手动摆臂已到播放位，执行暂存播放请求");
                finish_motion_at_target();
            }
            return;
        }

        const uint32_t motion_elapsed_ms = now - s_motion_started_ms;

        if (s_motion_phase == HallMotionPhase::Driving) {
            // 完整输出 220ms 脉冲。驱动期间只采集原始霍尔变化，不做“到位”判定，
            // 避免机械抖动或过早边沿导致播放状态提前切换。
            if (motion_elapsed_ms < SOLENOID_DRIVE_MS) {
                return;
            }

            // hall_control_tick() 早于 board_hw_solenoid_tick() 执行，因此在 220ms 到点时
            // 主动断电，随后从新的时间基准开始机械稳定与霍尔消抖确认。
            if (board_hw_solenoid_is_busy() && !board_hw_solenoid_stop()) {
                if (motion_elapsed_ms >= SOLENOID_TARGET_TIMEOUT_MS) {
                    fail_motion_timeout();
                }
                return;
            }

            begin_motion_confirmation(now);
        }

        if (s_motion_phase == HallMotionPhase::Confirming &&
            motion_target_confirmed(now)) {
            finish_motion_at_target();
            return;
        }

        if (motion_elapsed_ms >= SOLENOID_TARGET_TIMEOUT_MS) {
            fail_motion_timeout();
        }
        // 脉冲结束前不判定，脉冲结束后只接受经过独立消抖的目标电平。
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
    const bool hall_enabled = effective_hall_enabled(settings);

    if (!s_inited) {
        hall_control_begin();
    }

    if (!settings.solenoid_enabled) {
        if (!hall_enabled) {
            return false;
        }

        const PlayerPlaybackState playback_state = player_playback_state_get();
        if (playback_state == PlayerPlaybackState::Playing) {
            // 暂停请求不需要播放位门禁，继续由播放器直接暂停。
            return false;
        }

        if (!hall_control_is_near()) {
            // 已在播放位，允许播放器直接开始或恢复。
            return false;
        }

        LOGI("[HALL] 仅霍尔模式拒绝直接播放：当前=靠近，请手动拨到播放位");
        show_hall_notice("摆臂未在播放位", "请手动拨到播放位", WebNoticeLevel::Warning);
        return true;
    }

    // 电磁铁开启时，本次实体按键或 Web 请求始终由联动状态机接管；
    // 目标位置由播放器状态决定，而不是简单把摆臂翻到另一端：
    // - 正在播放：目标为靠近霍尔的停止位；
    // - 暂停或停止：目标为离开霍尔的播放位。
    if (!hall_actions_allowed()) {
        LOGW("[HALL] 当前状态不允许驱动摆臂");
        show_hall_notice("无法驱动摆臂", "请退出扫描或返回播放器", WebNoticeLevel::Warning);
        return true;
    }

    if (s_motion_target != HallMotionTarget::None || board_hw_solenoid_is_busy()) {
        LOGD("[HALL] 摆臂动作尚未结束，忽略重复播放/暂停请求");
        show_hall_notice("摆臂动作中", "请稍候", WebNoticeLevel::Info);
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
            show_hall_notice("播放状态切换失败", "请检查当前歌曲或播放源", WebNoticeLevel::Error);
        }
        return true;
    }

    const bool started = target == HallMotionTarget::Near
        ? board_hw_solenoid_move_hall_near(SOLENOID_DRIVE_MS)
        : board_hw_solenoid_move_hall_far(SOLENOID_DRIVE_MS);

    if (!started) {
        LOGE("[HALL] 电磁铁启动失败：目标=%s", motion_target_label(target));
        show_hall_notice("电磁铁启动失败", "请检查驱动芯片和I2C", WebNoticeLevel::Error);
        return true;
    }

    s_motion_target = target;
    s_motion_phase = HallMotionPhase::Driving;
    s_motion_started_ms = millis();
    s_motion_confirm_started_ms = 0;
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
    const bool hall_enabled = effective_hall_enabled(settings);

    if (!s_inited) {
        hall_control_begin();
    }

    if (!hall_enabled) {
        return HallPlayPositionRequestResult::Ready;
    }

    if (!callback) {
        LOGE("[HALL] 延迟起播请求缺少完成回调");
        return HallPlayPositionRequestResult::Failed;
    }

    if (!hall_actions_allowed()) {
        LOGW("[HALL] 当前状态不允许执行延迟起播摆臂动作");
        show_hall_notice("无法驱动摆臂", "请退出扫描或返回播放器", WebNoticeLevel::Warning);
        return HallPlayPositionRequestResult::Failed;
    }

    if (s_motion_target != HallMotionTarget::None || board_hw_solenoid_is_busy()) {
        LOGW("[HALL] 已有摆臂动作，拒绝新的延迟起播请求");
        show_hall_notice("摆臂动作中", "本次播放请求未执行", WebNoticeLevel::Warning);
        return HallPlayPositionRequestResult::Failed;
    }

    if (!hall_control_is_near()) {
        LOGI("[HALL] 摆臂已在播放位，可立即起播");
        return HallPlayPositionRequestResult::Ready;
    }

    if (!settings.solenoid_enabled) {
        // 仅霍尔模式：保留回调并等待用户手动把摆臂拨到播放位。
        // 不设置超时，不提前修改歌曲、分组或播放源。
        s_motion_target = HallMotionTarget::Far;
        s_motion_phase = HallMotionPhase::WaitingManual;
        s_motion_started_ms = 0;
        s_motion_confirm_started_ms = 0;
        s_motion_callback = callback;
        s_suppress_late_target_edge = false;
        LOGI("[HALL] 仅霍尔模式暂存播放请求：当前=靠近，等待手动拨到播放位");
        show_hall_notice("摆臂未在播放位", "请手动拨到播放位", WebNoticeLevel::Warning);
        return HallPlayPositionRequestResult::Started;
    }

    s_suppress_late_target_edge = false;
    if (!board_hw_solenoid_move_hall_far(SOLENOID_DRIVE_MS)) {
        LOGE("[HALL] 延迟起播驱动失败：目标=离开");
        show_hall_notice("电磁铁启动失败", "歌曲未播放", WebNoticeLevel::Error);
        return HallPlayPositionRequestResult::Failed;
    }

    s_motion_target = HallMotionTarget::Far;
    s_motion_phase = HallMotionPhase::Driving;
    s_motion_started_ms = millis();
    s_motion_confirm_started_ms = 0;
    s_motion_callback = callback;
    LOGI("[HALL] 延迟起播驱动摆臂：当前=靠近 目标=离开 脉冲=%lums",
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
    // 只有电磁铁联动模式把摆臂位置作为所有恢复路径的强约束。
    // 仅霍尔模式只门禁用户主动播放入口，避免影响闹钟、快照等内部恢复流程。
    return s_inited &&
           settings.solenoid_enabled &&
           effective_hall_enabled(settings) &&
           hall_control_is_near();
}

bool hall_control_pause_latched()
{
    return s_pause_latched;
}
