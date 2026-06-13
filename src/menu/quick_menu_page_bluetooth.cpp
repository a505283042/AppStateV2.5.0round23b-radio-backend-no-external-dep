#include "menu/quick_menu_page_bluetooth.h"

#include "hal/board_hw_control.h"
#include "utils/log.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {

#define MENU_COUNT(arr) static_cast<uint8_t>(sizeof(arr) / sizeof((arr)[0]))

volatile bool s_bt_reboot_in_progress = false;
bool s_bt_reboot_restore_wakeup = false;

bool bt_can_use_control_items()
{
    return board_hw_get_bt_power() && !s_bt_reboot_in_progress;
}

const char* value_bt_power()
{
    if (s_bt_reboot_in_progress) {
        return "重启中";
    }

    return board_hw_get_bt_power() ? "开" : "关";
}

const char* value_bt_wakeup()
{
    if (s_bt_reboot_in_progress) {
        return "重启中";
    }

    if (!board_hw_get_bt_power()) {
        return "需上电";
    }

    return board_hw_get_bt_wakeup() ? "保持" : "释放";
}

const char* value_bt_switch()
{
    if (s_bt_reboot_in_progress) {
        return "重启中";
    }

    return board_hw_get_bt_power() ? "按一下" : "需上电";
}

const char* value_bt_reboot()
{
    if (s_bt_reboot_in_progress) {
        return "重启中";
    }

    return board_hw_get_bt_power() ? "执行" : "需上电";
}

bool action_toggle_bt_power()
{
    if (s_bt_reboot_in_progress) {
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

bool action_toggle_bt_wakeup()
{
    if (!bt_can_use_control_items()) {
        LOGW("[蓝牙] 唤醒切换已忽略：电源关闭或正在重启");
        return false;
    }

    const bool next_enabled = !board_hw_get_bt_wakeup();

    // WKP 是蓝牙模块唤醒控制脚，菜单中只切换控制电平，便于实机验证。
    return board_hw_set_bt_wakeup(next_enabled);
}

void bt_reboot_task(void*)
{
    // 蓝牙重启需要等待断电保持时间，放到独立任务里，避免阻塞菜单/按键任务。
    LOGI("[蓝牙] 开始重启");
    const bool power_off_ok = board_hw_set_bt_power(false);

    vTaskDelay(pdMS_TO_TICKS(300));

    if (power_off_ok) {
        const bool power_on_ok = board_hw_set_bt_power(true);
        const bool wakeup_ok = board_hw_set_bt_wakeup(s_bt_reboot_restore_wakeup);
        LOGI("[蓝牙] 重启完成：电源开启=%d 唤醒恢复=%d", power_on_ok ? 1 : 0, wakeup_ok ? 1 : 0);
    } else {
        LOGW("[蓝牙] 重启失败：关闭电源步骤失败");
    }

    s_bt_reboot_in_progress = false;
    vTaskDelete(nullptr);
}

bool action_reboot_bt_module()
{
    if (!board_hw_get_bt_power()) {
        LOGW("[蓝牙] 重启已忽略：电源关闭");
        return false;
    }

    if (s_bt_reboot_in_progress) {
        LOGW("[蓝牙] 重启已忽略：已经在重启");
        return false;
    }

    s_bt_reboot_restore_wakeup = board_hw_get_bt_wakeup();
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
        LOGW("[蓝牙] 创建重启任务失败");
        return false;
    }

    return true;
}

const QuickMenuItem BLUETOOTH_ITEMS[] = {
    {"蓝牙电源", QuickMenuItemType::Toggle, QuickMenuPage::Bluetooth, "", value_bt_power, action_toggle_bt_power, true, false},
    {"蓝牙角色查询", QuickMenuItemType::Placeholder, QuickMenuPage::Bluetooth, "占位", nullptr, nullptr, false, true},
    {"输入模式查询", QuickMenuItemType::Placeholder, QuickMenuPage::Bluetooth, "占位", nullptr, nullptr, false, true},
    {"蓝牙名称查询", QuickMenuItemType::Placeholder, QuickMenuPage::Bluetooth, "占位", nullptr, nullptr, false, true},
    {"允许配对", QuickMenuItemType::Placeholder, QuickMenuPage::Bluetooth, "待接入", nullptr, nullptr, false, true},
    {"SW配对确认", QuickMenuItemType::Action, QuickMenuPage::Bluetooth, "", value_bt_switch, action_pulse_bt_switch, true, false},
    {"WKP休眠唤醒", QuickMenuItemType::Toggle, QuickMenuPage::Bluetooth, "", value_bt_wakeup, action_toggle_bt_wakeup, true, false},
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