#include "app_state.h"         /* 包含应用状态管理模块 */
#include "boot_state.h"         /* 包含启动状态模块 */
#include "player_state.h"       /* 包含播放器状态模块 */
#include "player_list_select.h" /* 包含列表选择模块 */
#include "player_control.h"
#include <Arduino.h>
#include "nfc/nfc_admin_state.h" /* 包含NFC管理状态模块 */
#include "nfc/nfc.h"           /* 包含NFC模块 */
#include "keys/keys.h"                /* 包含按键处理模块 */
#include "app_flags.h"
#include "ui/ui.h"
#include "utils/log.h"
#include "audio/audio_service.h"
#include "audio/audio_file.h"
#include "storage/storage_catalog_v3.h"
#include "player_recover.h"
#include "web/web_server.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

volatile bool g_rescan_done = false; // 扫描完成标志
volatile bool g_rescanning = false; // 扫描中标志
volatile bool g_rescan_success = false; // 扫描成功标志
volatile bool g_abort_scan = false; // 扫描中断标志
volatile bool g_random_play = false; // 随机播放标志
volatile play_mode_t g_play_mode = PLAY_MODE_ALL_SEQ;  // 播放模式

static TaskHandle_t s_rescan_task = nullptr; // 扫描任务句柄

// 扫描任务入口
static void app_rescan_task_entry(void* )
{
    bool success = false;

    player_control_mark_manual_stop();
    audio_service_stop(true);

    uint32_t start = millis();
    while (audio_service_is_playing() && (millis() - start) < 1000) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    audio_file_invalidate_dir_cache();
    success = storage_catalog_v3_rebuild("/Music", "/System/music_index_v3.bin");

    g_rescan_success = success;
    g_rescan_done = true;
    s_rescan_task = nullptr;
    vTaskDelete(nullptr);
}

/* 全局应用状态变量，初始值为启动状态 */
app_state_t g_app_state = STATE_BOOT;

/* 初始化应用状态为启动状态 */
void app_state_init(void)
{
    g_app_state = STATE_BOOT;
    keys_init(); /* 初始化按键处理模块 */
}

/* 根据当前应用状态执行相应的状态处理函数 */
void app_state_update(void)
{
    // 按键处理也需要高频调用，确保响应及时
    keys_update();
    web_server_poll();

    switch (g_app_state) {
        case STATE_BOOT:        /* 如果是启动状态，则运行启动状态处理函数 */
            boot_state_run();
            break;

        case STATE_PLAYER:      /* 如果是播放器状态，则运行播放器状态处理函数 */
            player_state_run();
            break;

        case STATE_NFC_ADMIN:   /* 如果是NFC管理状态，则运行NFC管理状态处理函数 */
            nfc_admin_state_run();
            break;

        default:                /* 默认情况下不执行任何操作 */
            break;
    }
}

/* 请求进入 NFC 管理状态 */
bool app_request_enter_nfc_admin()
{
    // 只允许从播放器主状态进入
    if (g_app_state != STATE_PLAYER) {
        LOGI("[APP] enter NFC admin denied: not in player state");
        return false;
    }

    // 扫描中不允许进入
    if (g_rescanning) {
        LOGI("[APP] enter NFC admin denied: rescanning");
        return false;
    }

    // 列表选择模式不允许进入
    if (player_list_select_is_active()) {
        LOGI("[APP] enter NFC admin denied: list select mode");
        return false;
    }

    LOGI("[APP] entering NFC admin");

    // 冻结一下 UI 渲染，避免切页时和旧界面打架
    ui_hold_render(true);

    // 同步按键状态到硬件，避免"长按进入后，松手又触发别的键行为"
    keys_sync_to_hw_state();

    // 清理旧的 NFC 待处理事件，防止刚进 admin 就吃到上一次遗留 UID
    String dummy;
    while (nfc_take_last_uid(dummy)) {
        // drain pending uid
    }

    // 先切状态，再调用 nfc_admin_state_enter() 让它自己处理 UI
    g_app_state = STATE_NFC_ADMIN;
    nfc_admin_state_enter();

    ui_hold_render(false);
    return true;
}

/* 请求退出 NFC 管理状态 */
void app_request_exit_nfc_admin()
{
    LOGI("[APP] exiting NFC admin");

    ui_hold_render(true);

    keys_sync_to_hw_state();

    nfc_admin_state_exit();
    g_app_state = STATE_PLAYER;

    ui_return_to_player();

    ui_hold_render(false);

    if (nfc_admin_state_consume_resume_request()) {
        LOGI("[APP] NFC admin exit: resume current track");
        player_toggle_play();
    }
}

// 请求启动扫描
bool app_request_start_rescan()
{
    if (g_app_state != STATE_PLAYER) {
        LOGI("[APP] start rescan denied: not in player state");
        return false;
    }
    if (g_rescanning || s_rescan_task != nullptr) {
        LOGI("[APP] start rescan denied: already rescanning");
        return false;
    }
    if (player_list_select_is_active()) {
        LOGI("[APP] start rescan denied: list select mode");
        return false;
    }

    player_recover_prepare_rescan_restore_current();
    g_abort_scan = false;
    g_rescan_done = false;
    g_rescan_success = false;
    g_rescanning = true;

    BaseType_t ok = xTaskCreate(
        app_rescan_task_entry,
        "rescan_v3",
        8192,
        nullptr,
        1,
        &s_rescan_task);

    if (ok != pdPASS) {
        LOGE("[APP] Failed to create rescan task");
        s_rescan_task = nullptr;
        g_rescanning = false;
        return false;
    }

    return true;
}

// 请求取消扫描
bool app_request_cancel_rescan()
{
    if (!g_rescanning) {
        LOGI("[APP] cancel rescan ignored: not rescanning");
        return false;
    }
    if (!g_abort_scan) {
        g_abort_scan = true;
        LOGI("[APP] rescan cancel requested");
    }
    return true;
}
