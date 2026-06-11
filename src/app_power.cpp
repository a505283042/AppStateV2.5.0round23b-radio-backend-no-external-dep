#include "app_power.h"

#include <Arduino.h>

#include "audio/audio_service.h"
#include "hal/board_hw_control.h"
#include "menu/quick_menu.h"
#include "nfc/nfc_binding.h"
#include "player_snapshot.h"
#include "ui/ui_power_prompt.h"
#include "web/web_settings.h"
#include "utils/log.h"

void app_power_save_and_shutdown()
{
    LOGI("[POWER] save and shutdown requested");

    // 如果是从未来某个菜单入口触发，也先退出菜单。
    quick_menu_exit();

    // 确保能看到关机提示。
    (void)board_hw_set_backlight(true);
    ui_power_show_shutdown_stage("正在保存设置", "请稍候...");
    delay(150);

    // 先暂停音频、静音功放，降低关机爆音风险。
    audio_service_pause();
    (void)board_hw_set_amp_mute(true);

    const bool snapshot_ok = player_snapshot_save_to_nvs();
    const bool web_ok = web_settings_save();

    // NFC 绑定在刷卡确认时只写内存并标记 dirty。
    // 真正写 TF 前必须停止 AudioTask 读卡，避免播放中写 /System/nfc_map.txt 抢 SD 锁。
    audio_service_stop(true);

    const bool nfc_ok = nfc_binding_flush_if_dirty("/System/nfc_map.txt");

    LOGI("[POWER] save result snapshot=%d web=%d nfc=%d",
         snapshot_ok ? 1 : 0,
         web_ok ? 1 : 0,
         nfc_ok ? 1 : 0);

    if (snapshot_ok && web_ok && nfc_ok) {
        ui_power_show_shutdown_stage("保存完成", "正在关机...");
    } else {
        ui_power_show_shutdown_stage("部分保存失败", "仍将关机...");
    }

    delay(500);

    // 可选：关闭高功耗外设。
    (void)board_hw_set_bt_power(false);
    (void)board_hw_set_amp_shutdown(true);

    delay(80);

    board_hw_power_off();

    // 如果硬件没有真正断电，就停在提示页，避免继续运行。
    while (true) {
        delay(1000);
    }
}