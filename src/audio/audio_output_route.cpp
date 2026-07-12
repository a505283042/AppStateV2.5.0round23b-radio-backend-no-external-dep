#include "audio/audio_output_route.h"

#include "audio/audio.h"
#include "audio/audio_service.h"
#include "hal/bt62sp_uart_debug.h"
#include "hal/board_hw_control.h"
#include "ui/ui.h"
#include "utils/log.h"

namespace {

// 默认使用功放输出；3.5 耳机/Line out 始终常通，不受本状态控制。
volatile uint8_t s_route = static_cast<uint8_t>(AudioOutputRoute::Speaker);

// 蓝牙发射模式下，BT62SP 的 L/R 是模拟输入。播放器音量过高会让模块输入级过载，
// 蓝牙音箱端听起来会发糊。因此进入蓝牙发射后固定播放器音量，只调 BT62SP AT+VOL。
static constexpr uint8_t BT_TX_PLAYER_FIXED_VOLUME = 20;
static constexpr uint8_t BT_TX_VOLUME_DEFAULT = 70;

volatile uint8_t s_bt_tx_volume = BT_TX_VOLUME_DEFAULT;
volatile uint8_t s_saved_normal_volume = 80;
bool s_bt_tx_volume_policy_active = false;

AudioOutputRoute current_route()
{
    return static_cast<AudioOutputRoute>(s_route);
}

bool set_route(AudioOutputRoute route)
{
    // 路由状态和相关硬件切换统一交给 AudioTask 串行执行。
    return audio_service_set_output_route(route, true);
}

bool route_allows_amp()
{
    return current_route() == AudioOutputRoute::Speaker;
}

void apply_bluetooth_tx_volume_policy()
{
    if (!s_bt_tx_volume_policy_active) {
        s_saved_normal_volume = audio_get_volume();
        s_bt_tx_volume_policy_active = true;
        LOGD("[音量] 进入蓝牙发射：保存播放器音量=%u", (unsigned)s_saved_normal_volume);
    }

    audio_set_volume(BT_TX_PLAYER_FIXED_VOLUME);
    ui_set_volume(s_bt_tx_volume);
    (void)bt62sp_uart_debug_set_volume(s_bt_tx_volume);
    LOGD("[音量] 蓝牙发射：播放器固定=%u BT62SP音量=%u",
         (unsigned)BT_TX_PLAYER_FIXED_VOLUME,
         (unsigned)s_bt_tx_volume);
}

void restore_normal_volume_policy_if_needed()
{
    if (!s_bt_tx_volume_policy_active) {
        return;
    }

    const uint8_t restore = s_saved_normal_volume > 100 ? 100 : s_saved_normal_volume;
    s_bt_tx_volume_policy_active = false;
    audio_set_volume(restore);
    ui_set_volume(restore);
    LOGD("[音量] 离开蓝牙发射：恢复播放器音量=%u", (unsigned)restore);
}

} // namespace

AudioOutputRoute audio_output_route_get()
{
    return current_route();
}

const char* audio_output_route_label()
{
    if (audio_output_route_is_headphone_only()) {
        return "仅耳机";
    }

    if (audio_output_route_is_bluetooth_tx()) {
        return "耳机+蓝牙";
    }

    return "耳机+功放";
}

bool audio_output_route_is_headphone_only()
{
    return current_route() == AudioOutputRoute::HeadphoneOnly;
}

bool audio_output_route_is_speaker()
{
    return current_route() == AudioOutputRoute::Speaker;
}

bool audio_output_route_is_bluetooth_tx()
{
    return current_route() == AudioOutputRoute::BluetoothTx;
}

uint8_t audio_output_route_bluetooth_tx_player_fixed_volume()
{
    return BT_TX_PLAYER_FIXED_VOLUME;
}

uint8_t audio_output_route_bluetooth_tx_volume()
{
    return s_bt_tx_volume;
}

uint8_t audio_output_route_get_user_volume()
{
    if (audio_output_route_is_bluetooth_tx()) {
        return s_bt_tx_volume;
    }
    return audio_get_volume();
}

bool audio_output_route_set_user_volume(uint8_t value)
{
    return audio_service_set_user_volume(value, true);
}

bool audio_output_route_step_user_volume(int delta)
{
    int v = static_cast<int>(audio_output_route_get_user_volume()) + delta;
    if (v < 0) {
        v = 0;
    }
    if (v > 100) {
        v = 100;
    }
    return audio_output_route_set_user_volume(static_cast<uint8_t>(v));
}

bool audio_output_route_select_headphone_only()
{
    return set_route(AudioOutputRoute::HeadphoneOnly);
}

bool audio_output_route_select_speaker()
{
    return set_route(AudioOutputRoute::Speaker);
}

bool audio_output_route_select_bluetooth_tx()
{
    return set_route(AudioOutputRoute::BluetoothTx);
}

bool audio_output_route_enforce()
{
    return audio_service_set_output_route(current_route(), true);
}

bool audio_output_route_set_amp_mute(bool enabled)
{
    return audio_service_set_amp_mute(enabled, true);
}

bool audio_output_route_set_amp_shutdown(bool enabled)
{
    return audio_service_set_amp_shutdown(enabled, true);
}

bool audio_output_route_apply_from_audio_task(AudioOutputRoute route)
{
    // 只有 AudioTask 可以更新路由并直接操作音频输出硬件。
    s_route = static_cast<uint8_t>(route);

    bool ok = true;

    if (audio_output_route_is_bluetooth_tx()) {
        // 耳机+蓝牙：功放必须保持关闭，避免喇叭与蓝牙同时出声。
        ok = board_hw_set_amp_mute(true) && ok;
        ok = board_hw_set_amp_shutdown(true) && ok;
        ok = board_hw_set_bt_mode(true) && ok;
        ok = board_hw_set_bt_power(true) && ok;
        apply_bluetooth_tx_volume_policy();
        LOGD("[音频输出] 路线=耳机+蓝牙，功放静音并关断，蓝牙切到发射模式并上电");
        return ok;
    }

    if (audio_output_route_is_headphone_only()) {
        restore_normal_volume_policy_if_needed();
        // 仅耳机：关闭蓝牙，同时保持功放静音和关断。
        ok = board_hw_set_bt_power(false) && ok;
        ok = board_hw_set_bt_mode(false) && ok;
        ok = board_hw_set_amp_mute(true) && ok;
        ok = board_hw_set_amp_shutdown(true) && ok;
        LOGD("[音频输出] 路线=仅耳机，功放静音并关断，蓝牙关闭并回到接收模式");
        return ok;
    }

    restore_normal_volume_policy_if_needed();
    // 切到功放时先解除关断但继续静音。是否取消静音由 AudioTask 根据播放/暂停状态决定。
    ok = board_hw_set_bt_power(false) && ok;
    ok = board_hw_set_bt_mode(false) && ok;
    ok = board_hw_set_amp_mute(true) && ok;
    ok = board_hw_set_amp_shutdown(false) && ok;
    LOGD("[音频输出] 路线=耳机+功放，功放已解除关断并保持静音");
    return ok;
}

bool audio_output_route_set_amp_mute_from_audio_task(bool enabled)
{
    if (!enabled && !route_allows_amp()) {
        // 非功放模式下，任何取消静音请求都改为保持静音。
        LOGD("[音频输出] 功放取消静音被阻止：路线=%s", audio_output_route_label());
        return board_hw_set_amp_mute(true);
    }

    return board_hw_set_amp_mute(enabled);
}

bool audio_output_route_set_amp_shutdown_from_audio_task(bool enabled)
{
    if (!enabled && !route_allows_amp()) {
        // 非功放模式下，任何释放关断请求都改为继续关断。
        LOGD("[音频输出] 功放解除关断被阻止：路线=%s", audio_output_route_label());
        bool ok = board_hw_set_amp_mute(true);
        ok = board_hw_set_amp_shutdown(true) && ok;
        return ok;
    }

    return board_hw_set_amp_shutdown(enabled);
}


bool audio_output_route_set_user_volume_from_audio_task(uint8_t value)
{
    if (value > 100) {
        value = 100;
    }

    if (audio_output_route_is_bluetooth_tx()) {
        s_bt_tx_volume = value;
        audio_set_volume(BT_TX_PLAYER_FIXED_VOLUME);
        ui_set_volume(value);
        (void)bt62sp_uart_debug_set_volume(value);
        LOGD("[音量] 蓝牙模块音量=%u 播放器固定=%u",
             (unsigned)value,
             (unsigned)BT_TX_PLAYER_FIXED_VOLUME);
        return true;
    }

    s_saved_normal_volume = value;
    audio_set_volume(value);
    ui_set_volume(value);
    LOGD("[音量] 播放器音量=%u", (unsigned)value);
    return true;
}
