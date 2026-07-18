#include "menu/quick_menu_page_bluetooth.h"

#include "hal/bluetooth_restart_controller.h"
#include "hal/board_hw_control.h"
#include "utils/log.h"

namespace {

#define MENU_COUNT(arr) static_cast<uint8_t>(sizeof(arr) / sizeof((arr)[0]))

bool bt_can_use_control_items()
{
    return board_hw_get_bt_power() && !bluetooth_restart_is_in_progress();
}

const char* value_bt_power()
{
    if (bluetooth_restart_is_in_progress()) {
        return "重启中";
    }

    return board_hw_get_bt_power() ? "开" : "关";
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

const char* value_bt_switch()
{
    if (bluetooth_restart_is_in_progress()) {
        return "重启中";
    }

    return board_hw_get_bt_power() ? "按一下" : "需上电";
}

const char* value_bt_reboot()
{
    if (bluetooth_restart_is_in_progress()) {
        return "重启中";
    }

    return board_hw_get_bt_power() ? "执行" : "需上电";
}

bool action_toggle_bt_power()
{
    if (bluetooth_restart_is_in_progress()) {
        LOGW("[蓝牙] 电源切换已忽略：正在重启");
        return false;
    }

    const bool next_enabled = !board_hw_get_bt_power();

    // 这里只控制蓝牙模块电源保持脚，不改变播放器当前音频源。
    return board_hw_set_bt_power(next_enabled);
}

bool action_pulse_bt_switch()
{
    if (!bt_can_use_control_items()) {
        LOGW("[蓝牙] 切换脉冲已忽略：电源关闭或正在重启");
        return false;
    }

    // 模拟按一下蓝牙模块 SW 脚，用于进入/确认配对流程。
    return board_hw_pulse_bt_switch(200);
}

bool action_toggle_bt_mode()
{
    if (bluetooth_restart_is_in_progress()) {
        LOGW("[蓝牙] 模式切换已忽略：正在重启");
        return false;
    }

    const bool next_enabled = !board_hw_get_bt_mode();

    // BT62SP 的 MODE_CTRL 由 ESP32 IO45 直接控制；有效电平可在硬件层统一调整。
    return board_hw_set_bt_mode(next_enabled);
}

bool action_reboot_bt_module()
{
    return bluetooth_restart_request(
        BluetoothRestartPolicy::PreserveCurrentMode);
}

const QuickMenuItem BLUETOOTH_ITEMS[] = {
    {"蓝牙电源", QuickMenuItemType::Toggle, QuickMenuPage::Bluetooth, "", value_bt_power, action_toggle_bt_power, true, false},
    {"蓝牙模式", QuickMenuItemType::Toggle, QuickMenuPage::Bluetooth, "", value_bt_mode, action_toggle_bt_mode, true, false},
    {"连接状态", QuickMenuItemType::Status, QuickMenuPage::Bluetooth, "", value_bt_link, nullptr, true, false},
    {"蓝牙角色查询", QuickMenuItemType::Placeholder, QuickMenuPage::Bluetooth, "占位", nullptr, nullptr, false, true},
    {"输入模式查询", QuickMenuItemType::Placeholder, QuickMenuPage::Bluetooth, "占位", nullptr, nullptr, false, true},
    {"蓝牙名称查询", QuickMenuItemType::Placeholder, QuickMenuPage::Bluetooth, "占位", nullptr, nullptr, false, true},
    {"允许配对", QuickMenuItemType::Placeholder, QuickMenuPage::Bluetooth, "待接入", nullptr, nullptr, false, true},
    {"SW配对确认", QuickMenuItemType::Action, QuickMenuPage::Bluetooth, "", value_bt_switch, action_pulse_bt_switch, true, false},
    {"蓝牙重启", QuickMenuItemType::Action, QuickMenuPage::Bluetooth, "", value_bt_reboot, action_reboot_bt_module, true, false},
    {"返回", QuickMenuItemType::Back, QuickMenuPage::Root, "", nullptr, nullptr, true, false},
};

} // namespace

const QuickMenuPageDef& quick_menu_get_bluetooth_page()
{
    static const QuickMenuPageDef page = {
        "蓝牙设置",
        QuickMenuPage::Bluetooth,
        QuickMenuPage::Root,
        BLUETOOTH_ITEMS,
        MENU_COUNT(BLUETOOTH_ITEMS),
    };

    return page;
}