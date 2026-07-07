#include "menu/quick_menu_page_audio_output.h"

#include "audio/audio_output_route.h"
#include "hal/board_hw_control.h"
#include "utils/log.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {

#define MENU_COUNT(arr) static_cast<uint8_t>(sizeof(arr) / sizeof((arr)[0]))

volatile bool s_bt_reboot_in_progress = false;
bool s_bt_reboot_restore_mode = false;

bool bt_route_ready()
{
    return audio_output_route_is_bluetooth_tx() && board_hw_get_bt_power() && !s_bt_reboot_in_progress;
}

const char* value_output_path()
{
    return audio_output_route_label();
}

const char* value_switch_route()
{
    return s_bt_reboot_in_progress ? "重启中" : "执行";
}

const char* value_amp_power()
{
    // SHDN 为功放关断控制：true 表示功放被关断，false 表示允许工作。
    return board_hw_get_amp_shutdown() ? "关断" : "工作";
}

const char* value_amp_mute()
{
    // MUTE 为功放静音控制：true 表示静音，false 表示正常播放。
    return board_hw_get_amp_mute() ? "静音" : "播放";
}

const char* value_bt_pair()
{
    return s_bt_reboot_in_progress ? "重启中" : "按一下";
}

const char* value_bt_mode()
{
    if (s_bt_reboot_in_progress) {
        return "重启中";
    }

    return board_hw_get_bt_mode() ? "发射" : "接收";
}

const char* value_bt_link()
{
    if (!board_hw_get_bt_power()) {
        return "未上电";
    }

    bool linked = false;
    if (!board_hw_read_bt_link(&linked)) {
        return "读取失败";
    }

    return linked ? "已连接" : "未连接";
}

const char* value_bt_reboot()
{
    return s_bt_reboot_in_progress ? "重启中" : "执行";
}

bool action_select_headphone_only()
{
    if (s_bt_reboot_in_progress) {
        LOGW("[音频输出] select headphone 已忽略: bt 正在重启");
        return false;
    }

    return audio_output_route_select_headphone_only();
}

bool action_select_speaker()
{
    if (s_bt_reboot_in_progress) {
        LOGW("[音频输出] select speaker 已忽略: bt 正在重启");
        return false;
    }

    return audio_output_route_select_speaker();
}

bool action_select_bluetooth()
{
    if (s_bt_reboot_in_progress) {
        LOGW("[音频输出] select bluetooth 已忽略: bt 正在重启");
        return false;
    }

    return audio_output_route_select_bluetooth_tx();
}

bool action_toggle_amp_mute()
{
    if (!audio_output_route_is_speaker()) {
        LOGW("[音频输出] amp mute 已忽略: 路线 is bluetooth tx");
        return false;
    }

    const bool next_mute = !board_hw_get_amp_mute();
    return audio_output_route_set_amp_mute(next_mute);
}

bool action_pulse_bt_switch()
{
    if (!bt_route_ready()) {
        LOGW("[音频输出] bt pair 已忽略: 路线 not bluetooth or 正在重启");
        return false;
    }

    // 模拟按一下蓝牙模块 SW 脚，用于进入/确认配对流程。
    return board_hw_pulse_bt_switch(200);
}

bool action_toggle_bt_mode()
{
    if (!bt_route_ready()) {
        LOGW("[音频输出] bt mode 已忽略: 路线 not bluetooth or 正在重启");
        return false;
    }

    const bool next_enabled = !board_hw_get_bt_mode();

    // BT62SP 的 MODE_CTRL 由 ESP32 IO45 直接控制；有效电平可在硬件层统一调整。
    return board_hw_set_bt_mode(next_enabled);
}

void bt_reboot_task(void*)
{
    // 蓝牙重启需要等待断电保持时间，放到独立任务里，避免阻塞菜单/按键任务。
    LOGI("[音频输出] bt reboot 启动");
    const bool power_off_ok = board_hw_set_bt_power(false);

    vTaskDelay(pdMS_TO_TICKS(300));

    if (power_off_ok && audio_output_route_is_bluetooth_tx()) {
        const bool mode_ok = board_hw_set_bt_mode(s_bt_reboot_restore_mode);
        const bool power_on_ok = board_hw_set_bt_power(true);
        (void)audio_output_route_enforce();
        LOGI("[音频输出] 蓝牙重启完成：电源开启=%d 模式恢复=%d", power_on_ok ? 1 : 0, mode_ok ? 1 : 0);
    } else if (!audio_output_route_is_bluetooth_tx()) {
        LOGW("[音频输出] bt reboot 已跳过 power on: 路线 changed");
    } else {
        LOGW("[音频输出] bt reboot 失败: power off step 失败");
    }

    s_bt_reboot_in_progress = false;
    vTaskDelete(nullptr);
}

bool action_reboot_bt_module()
{
    if (!audio_output_route_is_bluetooth_tx()) {
        LOGW("[音频输出] bt reboot 已忽略: 路线 not bluetooth tx");
        return false;
    }

    if (!board_hw_get_bt_power()) {
        LOGW("[音频输出] bt reboot 已忽略: power off");
        return false;
    }

    if (s_bt_reboot_in_progress) {
        LOGW("[音频输出] bt reboot 已忽略: al就绪 in progress");
        return false;
    }

    s_bt_reboot_restore_mode = board_hw_get_bt_mode();
    s_bt_reboot_in_progress = true;

    const BaseType_t created = xTaskCreate(
        bt_reboot_task,
        "bt_reboot",
        2048,
        nullptr,
        1,
        nullptr
    );

    if (created != pdPASS) {
        s_bt_reboot_in_progress = false;
        LOGW("[音频输出] bt reboot task 创建 失败");
        return false;
    }

    return true;
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
    {"切到耳机", QuickMenuItemType::Action, QuickMenuPage::AudioOutput, "", value_switch_route, action_select_headphone_only, true, false},
    {"切到功放", QuickMenuItemType::Action, QuickMenuPage::AudioOutput, "", value_switch_route, action_select_speaker, true, false},
    {"连接状态", QuickMenuItemType::Status, QuickMenuPage::AudioOutput, "", value_bt_link, nullptr, true, false},
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