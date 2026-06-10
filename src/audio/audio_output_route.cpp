#include "audio/audio_output_route.h"

#include "hal/board_hw_control.h"
#include "utils/log.h"

namespace {

// 默认使用功放输出；3.5 耳机/Line out 始终常通，不受本状态控制。
volatile uint8_t s_route = static_cast<uint8_t>(AudioOutputRoute::Speaker);

AudioOutputRoute current_route()
{
    return static_cast<AudioOutputRoute>(s_route);
}

bool set_route(AudioOutputRoute route)
{
    s_route = static_cast<uint8_t>(route);
    return audio_output_route_enforce();
}

bool route_allows_amp()
{
    return current_route() == AudioOutputRoute::Speaker;
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
    bool ok = true;

    if (audio_output_route_is_bluetooth_tx()) {
        // 耳机+蓝牙：功放必须保持关闭，避免音频任务把喇叭重新打开。
        ok = board_hw_set_amp_mute(true) && ok;
        ok = board_hw_set_amp_shutdown(true) && ok;
        ok = board_hw_set_bt_power(true) && ok;
        LOGI("[AUDIO_OUT] route=BluetoothTx amp muted/shutdown, bt on");
        return ok;
    }

    if (audio_output_route_is_headphone_only()) {
        // 仅耳机：关闭蓝牙发射，同时关闭功放，只保留硬件常通的耳机/Line out。
        ok = board_hw_set_bt_power(false) && ok;
        ok = board_hw_set_amp_mute(true) && ok;
        ok = board_hw_set_amp_shutdown(true) && ok;
        LOGI("[AUDIO_OUT] route=HeadphoneOnly amp muted/shutdown, bt off");
        return ok;
    }

    // 耳机+功放：关闭蓝牙发射，释放功放关断，并允许功放输出。
    ok = board_hw_set_bt_power(false) && ok;
    ok = board_hw_set_amp_shutdown(false) && ok;
    ok = board_hw_set_amp_mute(false) && ok;
    LOGI("[AUDIO_OUT] route=Speaker bt off, amp enabled");
    return ok;
}

bool audio_output_route_set_amp_mute(bool enabled)
{
    if (!enabled && !route_allows_amp()) {
        // 非功放模式下，任何取消静音请求都被改成保持静音。
        LOGW("[AUDIO_OUT] amp unmute blocked: route=%s", audio_output_route_label());
        return board_hw_set_amp_mute(true);
    }

    return board_hw_set_amp_mute(enabled);
}

bool audio_output_route_set_amp_shutdown(bool enabled)
{
    if (!enabled && !route_allows_amp()) {
        // 非功放模式下，任何释放关断请求都被改成继续关断。
        LOGW("[AUDIO_OUT] amp shutdown release blocked: route=%s", audio_output_route_label());
        bool ok = board_hw_set_amp_mute(true);
        ok = board_hw_set_amp_shutdown(true) && ok;
        return ok;
    }

    return board_hw_set_amp_shutdown(enabled);
}