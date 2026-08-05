#include "app_power.h"

#include <Arduino.h>

#include "audio/audio_output_route.h"
#include "audio/audio_service.h"
#include "hal/bluetooth_restart_controller.h"
#include "hal/board_hw_control.h"
#include "hal/ws2812_status.h"
#include "menu/quick_menu.h"
#include "nfc/nfc_binding.h"
#include "storage/system_paths.h"
#include "player_snapshot.h"
#include "player_source.h"
#include "player_list_select.h"
#include "ui/ui_power_prompt.h"
#include "ui/ui.h"
#include "web/web_settings.h"
#include "utils/log.h"

namespace {

static constexpr uint16_t SLEEP_PRESETS_MINUTES[] = {0, 15, 30, 60, 90};
static constexpr uint8_t SLEEP_PRESET_COUNT = sizeof(SLEEP_PRESETS_MINUTES) / sizeof(SLEEP_PRESETS_MINUTES[0]);

static bool s_sleep_timer_active = false;
static bool s_sleep_shutdown_started = false;
static uint16_t s_sleep_preset_minutes = 0;
static uint32_t s_sleep_deadline_ms = 0;
static uint32_t s_sleep_last_ui_refresh_bucket = UINT32_MAX;

// 设置睡眠关机后，屏幕在最后一次实体操作后保持一小段时间，再自动熄屏。
// 熄屏后按界面类按键只负责唤醒；再次唤醒后会重新计时。
static constexpr uint32_t SLEEP_SCREEN_OFF_DELAY_MS = 15000;
static bool s_sleep_screen_off_pending = false;
static bool s_sleep_last_backlight_enabled = true;
static uint32_t s_sleep_screen_off_deadline_ms = 0;

static bool s_pending_shutdown_active = false;
static uint32_t s_pending_shutdown_deadline_ms = 0;
static const char* s_pending_shutdown_reason = nullptr;

static uint8_t sleep_preset_index(uint16_t minutes)
{
    for (uint8_t i = 0; i < SLEEP_PRESET_COUNT; ++i) {
        if (SLEEP_PRESETS_MINUTES[i] == minutes) {
            return i;
        }
    }
    return 0;
}

} // namespace

void app_power_save_and_shutdown()
{
    LOGI("[电源] 保存 and shutdown requested");

    // 如果蓝牙正在重启，先禁止任务在断电等待结束后重新上电。
    (void)bluetooth_restart_cancel();

    // 关机流程开始即关闭状态灯，避免保存和断电阶段保持最后颜色。
    ws2812_status_off();

    // 已经进入关机流程后，睡眠定时不再重复触发。
    s_sleep_timer_active = false;
    s_sleep_preset_minutes = 0;
    s_sleep_deadline_ms = 0;
    s_sleep_screen_off_pending = false;
    s_sleep_screen_off_deadline_ms = 0;
    s_sleep_shutdown_started = true;

    // 如果是从未来某个菜单入口触发，也先退出菜单。
    quick_menu_exit();

    // 确保能看到关机提示。只显示一次简洁提示，避免关机 UI 阶段过多拖慢体验。
    (void)board_hw_set_backlight(true);
    ui_power_show_shutdown_stage("正在关机", "正在保存...");
    delay(200);

    // 暂停请求和功放静音均由 AudioTask 串行处理，避免关机线程直接碰音频硬件。
    (void)audio_service_pause(true);

    // 无论当前播放本地还是 NAS，都同时保存两套快照；当前音源只更新自己的那一套，另一套保持上次状态。
    const bool snapshot_ok = player_snapshot_save_to_nvs();

    const bool list_ok = player_list_select_flush_persistent_state();
    const bool web_ok = web_settings_save_if_dirty();

    // NFC 绑定在刷卡确认时只写内存并标记 dirty。
    // 真正写 TF 前必须停止 AudioTask 读卡，避免播放中写 /System/config/nfc_map.txt 抢 SD 锁。
    audio_service_stop(true);

    const bool nfc_ok = nfc_binding_flush_if_dirty(SystemPaths::kNfcMap);

    LOGI("[电源] 保存 result snapshot=%d list=%d 网页=%d nfc=%d",
         snapshot_ok ? 1 : 0,
         list_ok ? 1 : 0,
         web_ok ? 1 : 0,
         nfc_ok ? 1 : 0);

    if (snapshot_ok && list_ok && web_ok && nfc_ok) {
        ui_power_show_shutdown_stage("正在关机", "已保存");
    } else {
        ui_power_show_shutdown_stage("正在关机", "部分未保存");
    }

    delay(350);

    // 如果关机时仍处于蓝牙发射模式，先走正常退出路径。
    // 这样可以把当前 BT62SP 音量保存到 NVS，再关闭蓝牙电源。
    if (audio_output_route_is_bluetooth_tx()) {
        (void)audio_service_set_output_route(AudioOutputRoute::HeadphoneOnly, true);
    }

    // 可选：关闭高功耗外设。功放关断仍由 AudioTask 执行。
    (void)board_hw_set_bt_power(false);
    (void)audio_service_set_amp_shutdown(true, true);

    delay(80);

    board_hw_power_off();

    // 如果硬件没有真正断电，就停在提示页，避免继续运行。
    while (true) {
        delay(1000);
    }
}

void app_power_request_save_and_shutdown(const char* reason, uint32_t delay_ms)
{
    if (s_sleep_shutdown_started) {
        return;
    }

    s_pending_shutdown_active = true;
    s_pending_shutdown_deadline_ms = millis() + delay_ms;
    s_pending_shutdown_reason = reason;
    LOGI("[电源] 已预约保存关机：原因=%s 延迟=%lums",
         reason ? reason : "未指定",
         (unsigned long)delay_ms);
}

void app_power_sleep_timer_set_minutes(uint16_t minutes)
{
    if (minutes == 0) {
        app_power_sleep_timer_cancel();
        return;
    }

    s_sleep_preset_minutes = minutes;
    s_sleep_deadline_ms = millis() + (uint32_t)minutes * 60UL * 1000UL;
    s_sleep_timer_active = true;
    s_sleep_shutdown_started = false;

    s_sleep_last_backlight_enabled = board_hw_get_backlight();
    s_sleep_screen_off_pending = s_sleep_last_backlight_enabled;
    s_sleep_screen_off_deadline_ms = s_sleep_screen_off_pending
        ? millis() + SLEEP_SCREEN_OFF_DELAY_MS
        : 0;

    LOGI("[电源] 睡眠定时已设置：%u 分钟，屏幕将在 %lu 秒后自动关闭",
         (unsigned)minutes,
         (unsigned long)(SLEEP_SCREEN_OFF_DELAY_MS / 1000UL));
}

void app_power_sleep_timer_cancel()
{
    if (s_sleep_timer_active || s_sleep_preset_minutes != 0) {
        LOGI("[电源] 睡眠定时已取消");
    }

    s_sleep_timer_active = false;
    s_sleep_preset_minutes = 0;
    s_sleep_deadline_ms = 0;
    s_sleep_screen_off_pending = false;
    s_sleep_screen_off_deadline_ms = 0;
    s_sleep_last_backlight_enabled = board_hw_get_backlight();
    s_sleep_shutdown_started = false;
}

void app_power_sleep_timer_note_user_activity()
{
    if (!s_sleep_timer_active || s_sleep_shutdown_started) {
        return;
    }

    // 用户操作只延后熄屏，不负责点亮已经关闭的背光。
    // 熄屏下的按键语义仍由 keys 模块决定。
    if (!board_hw_get_backlight()) {
        return;
    }

    s_sleep_screen_off_pending = true;
    s_sleep_screen_off_deadline_ms = millis() + SLEEP_SCREEN_OFF_DELAY_MS;
    s_sleep_last_backlight_enabled = true;
}

bool app_power_sleep_timer_is_active()
{
    return s_sleep_timer_active;
}

uint32_t app_power_sleep_timer_remaining_seconds()
{
    if (!s_sleep_timer_active) {
        return 0;
    }

    const int32_t remain_ms = (int32_t)(s_sleep_deadline_ms - millis());
    if (remain_ms <= 0) {
        return 0;
    }

    return ((uint32_t)remain_ms + 999UL) / 1000UL;
}

uint16_t app_power_sleep_timer_preset_minutes()
{
    return s_sleep_timer_active ? s_sleep_preset_minutes : 0;
}

uint16_t app_power_sleep_timer_cycle_next()
{
    const uint8_t current = sleep_preset_index(app_power_sleep_timer_preset_minutes());
    const uint8_t next = (uint8_t)((current + 1) % SLEEP_PRESET_COUNT);
    const uint16_t minutes = SLEEP_PRESETS_MINUTES[next];

    app_power_sleep_timer_set_minutes(minutes);
    return minutes;
}

void app_power_sleep_timer_tick()
{
    if (s_sleep_shutdown_started) {
        return;
    }

    if (s_pending_shutdown_active) {
        if ((int32_t)(millis() - s_pending_shutdown_deadline_ms) >= 0) {
            const char* reason = s_pending_shutdown_reason;
            s_pending_shutdown_active = false;
            s_pending_shutdown_reason = nullptr;
            LOGI("[电源] 预约保存关机触发：%s", reason ? reason : "未指定");
            app_power_save_and_shutdown();
            return;
        }
    }

    if (!s_sleep_timer_active) {
        return;
    }

    const uint32_t now = millis();
    const bool backlight_enabled = board_hw_get_backlight();

    // 睡眠定时有效期间，屏幕每次被唤醒后重新开始 15 秒无操作息屏计时。
    if (backlight_enabled && !s_sleep_last_backlight_enabled) {
        s_sleep_screen_off_pending = true;
        s_sleep_screen_off_deadline_ms = now + SLEEP_SCREEN_OFF_DELAY_MS;
    } else if (!backlight_enabled) {
        s_sleep_screen_off_pending = false;
        s_sleep_screen_off_deadline_ms = 0;
    }
    s_sleep_last_backlight_enabled = backlight_enabled;

    if (s_sleep_screen_off_pending &&
        backlight_enabled &&
        (int32_t)(now - s_sleep_screen_off_deadline_ms) >= 0) {
        quick_menu_exit();
        (void)board_hw_set_backlight(false);
        s_sleep_screen_off_pending = false;
        s_sleep_screen_off_deadline_ms = 0;
        s_sleep_last_backlight_enabled = false;
        LOGI("[电源] 睡眠定时仍在运行，屏幕已自动关闭");
    }

    if ((int32_t)(now - s_sleep_deadline_ms) < 0) {
        return;
    }

    s_sleep_shutdown_started = true;
    LOGI("[电源] 睡眠定时到期，立即关机");
    app_power_save_and_shutdown();
}
