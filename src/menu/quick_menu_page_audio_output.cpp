#include "menu/quick_menu_page_audio_output.h"

#include "audio/audio_output_route.h"
#include "hal/bluetooth_restart_controller.h"
#include "hal/board_hw_control.h"
#include "hal/bt62sp_uart_debug.h"
#include "utils/log.h"

namespace {

#define MENU_COUNT(arr) static_cast<uint8_t>(sizeof(arr) / sizeof((arr)[0]))

bool bt_route_ready()
{
    return audio_output_route_is_bluetooth_tx() && board_hw_get_bt_power() && !bluetooth_restart_is_in_progress();
}

const char* value_output_path()
{
    return audio_output_route_label();
}

const char* value_switch_route()
{
    return bluetooth_restart_is_in_progress() ? "重启中" : "执行";
}

const char* value_amp_power()
{
    // 使用 MCP23017 实际引脚读回，而不是仅显示软件缓存。
    bool shutdown = false;
    if (!board_hw_read_amp_shutdown(&shutdown)) return "读取失败";
    return shutdown ? "关断" : "工作";
}

const char* value_amp_mute()
{
    bool muted = false;
    if (!board_hw_read_amp_mute(&muted)) return "读取失败";
    return muted ? "静音" : "播放";
}

const char* value_bt_pair()
{
    return bluetooth_restart_is_in_progress() ? "重启中" : "按一下";
}

const char* value_bt_mode()
{
    if (bluetooth_restart_is_in_progress()) {
        return "重启中";
    }

    return board_hw_get_bt_mode() ? "发射" : "接收";
}

const char* value_bt_link()
{
    if (bluetooth_restart_is_in_progress()) {
        return "重启中";
    }

    if (!board_hw_get_bt_power()) {
        return "未上电";
    }

    bool linked = false;
    if (!board_hw_read_bt_link(&linked)) {
        return "读取失败";
    }

    return linked ? "已连接" : "未连接";
}

const char* value_bt_pcm_level()
{
    static char buf[12];
    snprintf(buf,
             sizeof(buf),
             "%u/35",
             (unsigned)audio_output_route_bluetooth_tx_player_volume());
    return buf;
}

const char* value_bt_pcm_increase()
{
    return audio_output_route_bluetooth_tx_player_volume() >= 35
        ? "已最大"
        : "+1";
}

const char* value_bt_pcm_decrease()
{
    return audio_output_route_bluetooth_tx_player_volume() <= 1
        ? "已最小"
        : "-1";
}

bool action_increase_bt_pcm()
{
    return audio_output_route_step_bluetooth_tx_player_volume(+1);
}

bool action_decrease_bt_pcm()
{
    return audio_output_route_step_bluetooth_tx_player_volume(-1);
}

const char* value_bt_connected_device()
{
    static char buf[40];

    if (!bt_route_ready()) return "未连接";

    bool linked = false;
    if (!board_hw_read_bt_link(&linked)) return "读取失败";
    if (!linked) return "未连接";

    const Bt62spConnectedDeviceSnapshot device =
        bt62sp_uart_debug_connected_device_snapshot_get();

    if (device.state == Bt62spConnectedDeviceState::Querying) return "查询中";
    if (device.state == Bt62spConnectedDeviceState::Timeout) return "查询超时";
    if (device.state == Bt62spConnectedDeviceState::ParseError) return "解析失败";
    if (device.state == Bt62spConnectedDeviceState::ConnectedNoIdentity) {
        return "已连接·无名称记录";
    }
    if (device.state == Bt62spConnectedDeviceState::Connected) {
        snprintf(buf,
                 sizeof(buf),
                 "%s",
                 device.name[0] ? device.name : (device.mac[0] ? device.mac : "已连接"));
        return buf;
    }
    return "按下查询";
}

bool action_query_bt_connected_device()
{
    if (!bt_route_ready()) return false;

    bool linked = false;
    if (!board_hw_read_bt_link(&linked) || !linked) return false;
    return bt62sp_uart_debug_request_connected_device_query();
}

const char* value_bt_reboot()
{
    return bluetooth_restart_is_in_progress() ? "重启中" : "执行";
}

bool action_select_headphone_only()
{
    if (bluetooth_restart_is_in_progress()) {
        LOGW("[音频输出] 切换到耳机已忽略：蓝牙正在重启");
        return false;
    }

    return audio_output_route_select_headphone_only();
}

bool action_select_speaker()
{
    if (bluetooth_restart_is_in_progress()) {
        LOGW("[音频输出] 切换到功放已忽略：蓝牙正在重启");
        return false;
    }

    return audio_output_route_select_speaker();
}

bool action_select_bluetooth()
{
    if (bluetooth_restart_is_in_progress()) {
        LOGW("[音频输出] 切换到蓝牙已忽略：蓝牙正在重启");
        return false;
    }

    return audio_output_route_select_bluetooth_tx();
}

bool action_toggle_amp_mute()
{
    if (!audio_output_route_is_speaker()) {
        LOGW("[音频输出] 功放静音操作已忽略：当前不是功放路线");
        return false;
    }

    const bool next_mute = !board_hw_get_amp_mute();
    return audio_output_route_set_amp_mute(next_mute);
}

bool action_pulse_bt_switch()
{
    if (!bt_route_ready()) {
        LOGW("[音频输出] 蓝牙配对已忽略：路线不匹配或正在重启");
        return false;
    }

    // 模拟按一下蓝牙模块 SW 脚，用于进入/确认配对流程。
    return board_hw_pulse_bt_switch(200);
}

bool action_toggle_bt_mode()
{
    if (!bt_route_ready()) {
        LOGW("[音频输出] 蓝牙模式切换已忽略：路线不匹配或正在重启");
        return false;
    }

    const bool next_enabled = !board_hw_get_bt_mode();

    // BT62SP 的 MODE_CTRL 由 ESP32 IO45 直接控制；有效电平可在硬件层统一调整。
    return board_hw_set_bt_mode(next_enabled);
}

bool action_reboot_bt_module()
{
    return bluetooth_restart_request(
        BluetoothRestartPolicy::RequireBluetoothTxRoute);
}

const QuickMenuItem HEADPHONE_ITEMS[] = {
    {"输出路径", QuickMenuItemType::Status, QuickMenuPage::AudioOutput, "", value_output_path, nullptr, true, false},
    {"切到功放", QuickMenuItemType::Action, QuickMenuPage::AudioOutput, "", value_switch_route, action_select_speaker, true, false},
    {"切到蓝牙", QuickMenuItemType::Action, QuickMenuPage::AudioOutput, "", value_switch_route, action_select_bluetooth, true, false},
    {"返回", QuickMenuItemType::Back, QuickMenuPage::Root, "", nullptr, nullptr, true, false},
};

const QuickMenuItem SPEAKER_ITEMS[] = {
    {"输出路径", QuickMenuItemType::Status, QuickMenuPage::AudioOutput, "", value_output_path, nullptr, true, false},
    {"切到耳机", QuickMenuItemType::Action, QuickMenuPage::AudioOutput, "", value_switch_route, action_select_headphone_only, true, false},
    {"切到蓝牙", QuickMenuItemType::Action, QuickMenuPage::AudioOutput, "", value_switch_route, action_select_bluetooth, true, false},
    {"功放静音", QuickMenuItemType::Toggle, QuickMenuPage::AudioOutput, "", value_amp_mute, action_toggle_amp_mute, true, false},
    {"功放状态", QuickMenuItemType::Status, QuickMenuPage::AudioOutput, "", value_amp_power, nullptr, true, false},
    {"返回", QuickMenuItemType::Back, QuickMenuPage::Root, "", nullptr, nullptr, true, false},
};

const QuickMenuItem BLUETOOTH_TX_ITEMS[] = {
    {"输出路径", QuickMenuItemType::Status, QuickMenuPage::AudioOutput, "", value_output_path, nullptr, true, false},
    {"蓝牙PCM档位", QuickMenuItemType::Status, QuickMenuPage::AudioOutput, "", value_bt_pcm_level, nullptr, true, false},
    {"PCM增大", QuickMenuItemType::Action, QuickMenuPage::AudioOutput, "", value_bt_pcm_increase, action_increase_bt_pcm, true, false},
    {"PCM减小", QuickMenuItemType::Action, QuickMenuPage::AudioOutput, "", value_bt_pcm_decrease, action_decrease_bt_pcm, true, false},
    {"切到耳机", QuickMenuItemType::Action, QuickMenuPage::AudioOutput, "", value_switch_route, action_select_headphone_only, true, false},
    {"切到功放", QuickMenuItemType::Action, QuickMenuPage::AudioOutput, "", value_switch_route, action_select_speaker, true, false},
    {"连接状态", QuickMenuItemType::Status, QuickMenuPage::AudioOutput, "", value_bt_link, nullptr, true, false},
    {"已连设备", QuickMenuItemType::Action, QuickMenuPage::AudioOutput, "", value_bt_connected_device, action_query_bt_connected_device, true, false},
    {"功放状态", QuickMenuItemType::Status, QuickMenuPage::AudioOutput, "", value_amp_power, nullptr, true, false},
    {"功放静音", QuickMenuItemType::Status, QuickMenuPage::AudioOutput, "", value_amp_mute, nullptr, true, false},
    {"蓝牙模式", QuickMenuItemType::Toggle, QuickMenuPage::AudioOutput, "", value_bt_mode, action_toggle_bt_mode, true, false},
    {"蓝牙配对", QuickMenuItemType::Action, QuickMenuPage::AudioOutput, "", value_bt_pair, action_pulse_bt_switch, true, false},
    {"蓝牙重启", QuickMenuItemType::Action, QuickMenuPage::AudioOutput, "", value_bt_reboot, action_reboot_bt_module, true, false},
    {"返回", QuickMenuItemType::Back, QuickMenuPage::Root, "", nullptr, nullptr, true, false},
};

} // namespace

const QuickMenuPageDef& quick_menu_get_audio_output_page()
{
    if (audio_output_route_is_headphone_only()) {
        static const QuickMenuPageDef headphone_page = {
            "音频输出",
            QuickMenuPage::AudioOutput,
            QuickMenuPage::Root,
            HEADPHONE_ITEMS,
            MENU_COUNT(HEADPHONE_ITEMS),
        };

        return headphone_page;
    }

    if (audio_output_route_is_bluetooth_tx()) {
        static const QuickMenuPageDef bluetooth_page = {
            "音频输出",
            QuickMenuPage::AudioOutput,
            QuickMenuPage::Root,
            BLUETOOTH_TX_ITEMS,
            MENU_COUNT(BLUETOOTH_TX_ITEMS),
        };

        return bluetooth_page;
    }

    static const QuickMenuPageDef speaker_page = {
        "音频输出",
        QuickMenuPage::AudioOutput,
        QuickMenuPage::Root,
        SPEAKER_ITEMS,
        MENU_COUNT(SPEAKER_ITEMS),
    };

    return speaker_page;
}