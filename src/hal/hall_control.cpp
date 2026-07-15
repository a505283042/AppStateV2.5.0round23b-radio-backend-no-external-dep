#include "hal/hall_control.h"

#include <Arduino.h>

#include "app_flags.h"
#include "app_state.h"
#include "board/board_pins.h"
#include "player_control.h"
#include "utils/log.h"
#include "web/web_settings.h"

namespace {

// 当前硬件为低电平表示磁铁靠近。
constexpr int HALL_ACTIVE_LEVEL = LOW;
constexpr uint32_t HALL_DEBOUNCE_MS = 70;
constexpr uint32_t HALL_COMMAND_GUARD_MS = 150;

bool s_inited = false;
bool s_last_enabled = true;
int s_last_raw = HIGH;
int s_stable_level = HIGH;
uint32_t s_raw_change_ms = 0;
uint32_t s_last_command_ms = 0;
bool s_pause_latched = false;

bool hall_actions_allowed()
{
    return g_app_state == STATE_PLAYER && !g_rescanning;
}

void sync_input_level()
{
    const int level = digitalRead(PIN_HALL_OUT);
    const uint32_t now = millis();

    s_last_raw = level;
    s_stable_level = level;
    s_raw_change_ms = now;
    s_last_command_ms = 0;
}

bool command_guard_ready(uint32_t now)
{
    return s_last_command_ms == 0 ||
           static_cast<uint32_t>(now - s_last_command_ms) >= HALL_COMMAND_GUARD_MS;
}

void apply_stable_state(uint32_t now)
{
    if (!hall_actions_allowed()) {
        return;
    }

    const bool near = s_stable_level == HALL_ACTIVE_LEVEL;
    const PlayerPlaybackState state = player_playback_state_get();

    if (near) {
        // 靠近期间持续约束实际状态。这样即使切歌、NFC、Web 或异步网络起播，
        // 新音源真正开始后也会在下一轮立即暂停。
        if (state == PlayerPlaybackState::Playing && command_guard_ready(now)) {
            s_last_command_ms = now;
            if (player_set_paused(true, PlayerToggleTrigger::Hall)) {
                s_pause_latched = true;
                LOGI("[HALL] 磁铁靠近，已暂停播放");
            }
        }
        return;
    }

    // 离开时只恢复由霍尔造成的暂停。用户本来就手动暂停时不会被误恢复。
    if (!s_pause_latched) {
        return;
    }

    if (state == PlayerPlaybackState::Paused && command_guard_ready(now)) {
        s_last_command_ms = now;
        if (player_set_paused(false, PlayerToggleTrigger::Hall)) {
            s_pause_latched = false;
            LOGI("[HALL] 磁铁离开，已恢复播放");
        }
        return;
    }

    // 音源已经停止或被其他流程切走，不再用旧霍尔锁存强制重新起播。
    if (state == PlayerPlaybackState::Stopped) {
        s_pause_latched = false;
    }
}

} // namespace

void hall_control_begin()
{
    pinMode(PIN_HALL_OUT, INPUT_PULLUP);
    sync_input_level();
    s_pause_latched = false;
    s_last_enabled = web_settings_get().hall_control_enabled;
    s_inited = true;

    LOGI("[HALL] 初始化 GPIO%d，有效电平=%d，当前=%s",
         PIN_HALL_OUT,
         HALL_ACTIVE_LEVEL,
         hall_control_is_near() ? "靠近" : "离开");
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

    const bool enabled = web_settings_get().hall_control_enabled;
    if (!enabled) {
        if (s_last_enabled) {
            // 关闭控制时保持当前播放状态不变，并清除自动恢复归属。
            s_pause_latched = false;
            LOGI("[HALL] 控制已关闭，保持当前播放状态");
        }
        s_last_enabled = false;
        return;
    }

    if (!s_last_enabled) {
        s_last_enabled = true;
        s_last_command_ms = 0;
        LOGI("[HALL] 控制已开启，当前=%s", hall_control_is_near() ? "靠近" : "离开");
    }

    // 状态边沿和持续约束都调用同一幂等路径；没有状态变化时不会重复发命令。
    (void)stable_changed;
    apply_stable_state(now);
}

bool hall_control_is_near()
{
    return s_stable_level == HALL_ACTIVE_LEVEL;
}

bool hall_control_blocks_resume()
{
    return s_inited &&
           web_settings_get().hall_control_enabled &&
           hall_control_is_near();
}

bool hall_control_pause_latched()
{
    return s_pause_latched;
}
