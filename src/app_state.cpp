#include "app_state.h"         /* 包含应用状态管理模块 */
#include "boot_state.h"         /* 包含启动状态模块 */
#include "player_state.h"       /* 包含播放器状态模块 */
#include "player_list_select.h" /* 包含列表选择模块 */
#include "player_control.h"
#include "player_snapshot.h"
#include <Arduino.h>
#include "nfc/nfc_admin_state.h" /* 包含NFC管理状态模块 */
#include "nfc/nfc.h"           /* 包含NFC模块 */
#include "keys/keys.h"                /* 包含按键处理模块 */
#include "app_flags.h"
#include "app_power.h"
#include "ui/ui.h"
#include "utils/log.h"
#include "audio/audio_service.h"
#include "audio/audio_file.h"
#include "lyrics/lyrics.h"
#include "nfc/nfc_binding.h"
#include "radio/radio_catalog.h"
#include "net_music/net_music_catalog.h"
#include "storage/storage.h"
#include "storage/storage_catalog_v3.h"
#include "storage/storage_hotplug.h"
#include "player_assets.h"
#include "player_playlist.h"
#include "player_recover.h"
#include "player_source.h"
#include "web/web_server.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

volatile bool g_rescan_done = false; // 扫描完成标志
volatile bool g_rescanning = false; // 扫描中标志
volatile bool g_rescan_success = false; // 扫描成功标志
volatile bool g_abort_scan = false; // 扫描中断标志


volatile play_mode_t g_play_mode = PLAY_MODE_ALL_SEQ;  // 播放模式

static TaskHandle_t s_rescan_task = nullptr; // 扫描任务句柄
static bool s_tf_mount_restore_pending = false;

static void app_handle_tf_removed();
static void app_handle_tf_mounted();

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
    if (storage_is_ready()) {
        success = storage_catalog_v3_rebuild("/Music", "/System/music_index_v3.bin");
    } else {
        LOGW("[APP] rescan skipped: storage not ready");
        success = false;
    }

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
    storage_hotplug_init();
}

static void app_handle_tf_removed()
{
    LOGW("[APP] TF removed");
    s_tf_mount_restore_pending = false;

    const PlayerSourceState source = player_source_get();

    // TF 拔卡事件只发生一次，这里保存一次即可。
    // 不要放在 app_state_update() 高频循环里。
    if (source.type == PlayerSourceType::LOCAL_TRACK &&
        player_state_current_index() >= 0) {
        player_snapshot_save_to_nvs();
    }

    storage_mark_not_ready();
    g_abort_scan = true;

    player_list_select_reset();

    player_assets_invalidate_requests();
    player_assets_cancel_pending_cover_prefetch();
    player_assets_clear_primed_current_cover();
    player_assets_drop_primed_next_cover();
    player_assets_clear_deferred_current_cover_apply();
    player_assets_clear_web_cover_cache();

    // 本地音乐依赖 TF，必须停；网络电台不依赖 TF，可以继续播放。
    if (source.type != PlayerSourceType::NET_RADIO) {
        player_control_mark_manual_stop();
        audio_service_stop(true);
        player_source_reset();
        player_state_set_current_index(-1);
    }

    g_lyricsDisplay.clear();
    ui_cover_cache_invalidate();

    audio_file_invalidate_dir_cache();
    storage_catalog_v3_clear();
    net_music_catalog_clear();
    player_playlist_reset_state();
    nfc_binding_clear();

    storage_unmount();
    storage_clear_io_error();

    if (source.type != PlayerSourceType::NET_RADIO) {
        ui_show_player_placeholder("未插入TF卡", "插卡后自动加载");
    } else {
        ui_request_refresh_now();
    }
}

static void app_handle_tf_mounted()
{
    LOGD("[APP] TF mounted");

    const PlayerSourceState source_before_mount = player_source_get();

    const bool radio_active =
        source_before_mount.type == PlayerSourceType::NET_RADIO &&
        (audio_service_is_playing() || audio_service_is_paused());

    audio_file_prepare_music_root_cache();

    if (nfc_binding_load("/System/nfc_map.txt")) {
        LOGD("[APP] NFC bindings reloaded: %d entries", nfc_binding_count());
    } else {
        LOGD("[APP] no NFC bindings after TF mounted");
    }

    if (radio_catalog_load()) {
        LOGD("[APP] radio catalog reloaded: %d stations", (int)radio_catalog_count());
    } else {
        LOGW("[APP] radio catalog reload failed");
    }
    
    // TF 卡插入后不主动加载 NAS/HTTP 歌曲索引。
    // 后续进入 NAS 歌曲列表或 Web NAS 页面时再按需加载。
    net_music_catalog_clear();
    // TF 卡插入后不再自动重连 WiFi。
    // WiFi 总开关由 NVS 保存；如需联网，由用户在菜单中手动开启/重连。
    // 这样可以避免插卡后 WiFi 扫描/连接影响本地播放稳定性。
    LOGI("[APP] WiFi reconnect skipped after TF mounted");

    if (!storage_catalog_v3_load_or_rebuild("/Music", "/System/music_index_v3.bin")) {
        LOGE("[APP] catalog reload failed after TF mounted");
        ui_request_refresh_now();
        return;
    }

    player_playlist_reset_state();
    player_playlist_force_rebuild();

    // 关键修复：
    // 网络电台播放中插卡，只加载 TF 资源，不恢复本地歌曲快照。
    if (radio_active) {
        LOGI("[APP] TF mounted while radio active: skip local snapshot restore");
        s_tf_mount_restore_pending = false;
        ui_request_refresh_now();
        return;
    }

    // 插卡后重新从内部 NVS 读取播放器快照。
    // 因为开机无卡时，player_state 已经 enter 过一次，
    // 后续插卡不会再自动走首次进入播放器的恢复逻辑。
    s_tf_mount_restore_pending = false;

    if (player_snapshot_load_pending_from_nvs() &&
        player_snapshot_begin_restore_on_player_enter()) {
        s_tf_mount_restore_pending = true;
        LOGI("[APP] TF mounted: snapshot restore armed");
        ui_request_refresh_now();
        return;
    }

    if (storage_catalog_v3_track_count() > 0) {
        ui_show_player_placeholder("TF卡已就绪", "按播放键开始");
    } else {
        ui_show_player_placeholder("没有歌曲", "请检查 /Music 目录");
    }
}

/* 根据当前应用状态执行相应的状态处理函数 */
void app_state_update(void)
{
    // 按键处理也需要高频调用，确保响应及时
    keys_update();
    web_server_poll();

    // 睡眠关机定时器到点后走统一安全关机流程。
    app_power_sleep_timer_tick();

    const PlayerSourceState source = player_source_get();

    const bool local_audio_active =
        source.type == PlayerSourceType::LOCAL_TRACK &&
        (audio_service_is_playing() || audio_service_is_paused());

    // 无 CD 脚时，播放中不主动探测，避免干扰正常读卡；
    // 但一旦 AudioFile 上报 IO error，就必须允许热插拔探测抢占，
    // 否则播放器会把“拔卡”误判成“单曲播放失败”并连续 auto next。
    const bool storage_suspect = storage_has_recent_io_error();
    const bool allow_sd_probe =
        g_app_state != STATE_BOOT &&
        !g_rescanning &&
        (!local_audio_active || storage_suspect);

    const StorageHotplugEvent hotplug_event = storage_hotplug_poll(allow_sd_probe);
    if (hotplug_event == StorageHotplugEvent::CARD_REMOVED) {
        app_handle_tf_removed();
    } else if (hotplug_event == StorageHotplugEvent::CARD_MOUNTED) {
        app_handle_tf_mounted();
    }

    if (s_tf_mount_restore_pending) {
        const PlayerSourceState restore_source = player_source_get();

        if (restore_source.type == PlayerSourceType::NET_RADIO) {
            LOGW("[APP] cancel local snapshot restore: radio is active");
            s_tf_mount_restore_pending = false;
            return;
        }

        const PlayerSnapshotRestorePollResult restore_res =
            player_snapshot_poll_restore();

        if (restore_res == PLAYER_SNAPSHOT_RESTORE_WAITING) {
            return;
        }

        s_tf_mount_restore_pending = false;

        if (restore_res == PLAYER_SNAPSHOT_RESTORE_DONE) {
            LOGI("[APP] TF mounted: snapshot restore done");
            return;
        }

        LOGW("[APP] TF mounted: snapshot restore failed");

        // 恢复失败时不要自动从第一首播放，只提示用户手动开始。
        ui_request_refresh_now();
        return;
    }

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

/* 请求进入 NFC 管理状态并指定目标 */
bool app_request_enter_nfc_admin_with_target(const NfcAdminTarget& target)
{
    if (g_app_state != STATE_PLAYER) {
        LOGI("[APP] enter NFC admin denied: not in player state");
        return false;
    }

    if (g_rescanning) {
        LOGI("[APP] enter NFC admin denied: rescanning");
        return false;
    }

    if (player_list_select_is_active()) {
        LOGI("[APP] enter NFC admin denied: list select mode");
        return false;
    }

    LOGI("[APP] entering NFC admin with explicit target");

    ui_hold_render(true);
    keys_sync_to_hw_state();

    String dummy;
    while (nfc_take_last_uid(dummy)) {
    }

    nfc_admin_state_set_override_target(target);

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
    if (!storage_is_ready()) {
        LOGI("[APP] start rescan denied: storage not ready");
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