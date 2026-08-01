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
#include "storage/storage_config_writer.h"
#include "storage/storage_catalog_v3.h"
#include "storage/storage_hotplug.h"
#include "storage/system_paths.h"
#include "player_assets.h"
#include "player_playlist.h"
#include "player_recover.h"
#include "player_source.h"
#include "web/web_server.h"
#include "hal/board_hw_control.h"
#include "hal/hall_control.h"
#include "hal/ws2812_status.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static bool s_tf_mount_restore_pending = false;
static bool s_low_battery_shutdown_started = false;

static void app_handle_tf_removed();
static void app_handle_tf_mounted();

static bool app_prepare_tf_player_ui()
{
    if (!storage_catalog_v3_ready() || storage_catalog_v3_track_count() == 0) {
        ui_show_player_placeholder("没有歌曲", "请检查 /Music 目录");
        return false;
    }

    player_playlist_ensure_current();
    const int first_track = player_playlist_current_track_at(0);
    if (first_track < 0) {
        LOGE("[应用] TF 卡挂载后播放列表为空，无法进入播放器界面");
        ui_show_player_placeholder("播放列表加载失败", "请重试或重扫曲库");
        return false;
    }

    // 换卡后只预选当前播放列表第一首，不自动播放。
    player_control_mark_manual_stop();
    if (!player_prepare_local_track_ui(first_track)) {
        LOGE("[应用] TF 卡挂载后歌曲界面准备失败：索引=%d", first_track);
        ui_show_player_placeholder("歌曲信息加载失败", "请切换歌曲或重扫曲库");
        return false;
    }

    LOGI("[应用] TF 卡已就绪：已进入播放器界面，预选索引=%d", first_track);
    return true;
}

// 扫描任务入口
static void app_rescan_task_entry(void* )
{
    bool success = false;
    StorageCatalogRebuildSummary summary{};
    const AppRescanState rescan = app_rescan_state_get();
    StorageCatalogRebuildMode rebuild_mode =
        StorageCatalogRebuildMode::FastIncremental;
    if (rescan.mode == AppRescanMode::UltraFastIncremental) {
        rebuild_mode = StorageCatalogRebuildMode::UltraFastIncremental;
    } else if (rescan.mode == AppRescanMode::StrictIncremental) {
        rebuild_mode = StorageCatalogRebuildMode::StrictIncremental;
    } else if (rescan.mode == AppRescanMode::Full) {
        rebuild_mode = StorageCatalogRebuildMode::Full;
    }

    player_control_mark_manual_stop();
    audio_service_stop(true);

    uint32_t start = millis();
    while (audio_service_is_playing() && (millis() - start) < 1000) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    audio_file_invalidate_dir_cache();
    if (storage_is_ready()) {
        success = storage_catalog_v3_rebuild(
            "/Music",
            SystemPaths::kMusicIndexV3,
            rebuild_mode,
            &summary);
    } else {
        LOGW("[应用] rescan 已跳过: storage 未就绪");
        success = false;
    }

    if (success) {
        // Catalog 已经提交，结果页显示期间不再接受取消，避免“已保存却按取消处理”。
        app_rescan_mark_committed();
        UiScanSummary ui_summary{};
        ui_summary.full_scan = summary.full_scan;
        ui_summary.forced_full_scan = summary.forced_full_scan;
        ui_summary.strict_incremental = summary.strict_incremental;
        ui_summary.ultra_fast_incremental =
            summary.ultra_fast_incremental;
        ui_summary.unchanged = summary.unchanged;
        ui_summary.directories_skipped =
            summary.directories_skipped;
        ui_summary.discovered = summary.discovered;
        ui_summary.reused = summary.reused;
        ui_summary.added = summary.added;
        ui_summary.modified = summary.modified;
        ui_summary.deleted = summary.deleted;
        ui_summary.elapsed_ms = summary.elapsed_ms;
        ui_scan_complete(ui_summary);
    } else if (!app_rescan_should_abort()) {
        ui_scan_failed(rebuild_mode == StorageCatalogRebuildMode::Full);
    }

    app_rescan_mark_finished(success);
    vTaskDelete(nullptr);
}

/* 全局应用状态变量，初始值为启动状态 */
app_state_t g_app_state = STATE_BOOT;

/* 初始化应用状态为启动状态 */
void app_state_init(void)
{
    g_app_state = STATE_BOOT;
    keys_init(); /* 初始化按键处理模块 */
    hall_control_begin();
    (void)ws2812_status_begin();
    storage_hotplug_init();
}

static void app_handle_tf_removed()
{
    LOGW("[应用] TF 删除d");
    s_tf_mount_restore_pending = false;

    const PlayerSourceState source = player_source_get();

    // TF 拔卡事件只发生一次，这里把本地与 NAS 两套状态一起保存。
    // 当前是 NAS 时，本地快照保持上次状态；当前是本地时，NAS 快照保持上次状态。
    // 不要放在 app_state_update() 高频循环里。
    (void)player_snapshot_save_to_nvs();

    // 待写配置属于当前TF卡；拔卡后必须丢弃，禁止误写到随后插入的另一张卡。
    storage_config_discard_pending("TF卡已移除");
    storage_mark_not_ready();
    (void)app_rescan_request_abort();

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

    if (source.type != PlayerSourceType::NET_RADIO) {
        ui_show_player_placeholder("未插入TF卡", "插卡后自动加载");
    } else {
        ui_request_refresh_now();
    }
}

static void app_handle_tf_mounted()
{
    LOGD("[应用] TF 已挂载");

    // 热插拔重新挂载后也要创建分类目录并迁移旧路径。
    (void)storage_system_layout_prepare();

    const PlayerSourceState source_before_mount = player_source_get();

    const bool radio_active =
        source_before_mount.type == PlayerSourceType::NET_RADIO &&
        source_before_mount.radio_active;

    audio_file_prepare_music_root_cache();

    if (nfc_binding_load(SystemPaths::kNfcMap)) {
        LOGD("[应用] NFC bindings re加载ed: %d entries", nfc_binding_count());
    } else {
        LOGD("[应用] no NFC bindings after TF 已挂载");
    }

    if (radio_catalog_load()) {
        LOGD("[应用] 电台 目录 re加载ed: %d stations", (int)radio_catalog_count());
    } else {
        LOGW("[应用] 电台 目录 re加载 失败");
    }

    // TF 卡插入后不主动加载 NAS/HTTP 歌曲索引。
    // 这里只清空旧的 NAS 内存列表，然后重新读取很小的 base 文件。
    // 后续进入 NAS 歌曲列表或 Web NAS 页面时，再从 HTTP 下载 net_music.txt 到内存。
    net_music_catalog_clear();
    if (net_music_catalog_load_base()) {
        LOGD("[应用] NAS base 重新加载成功: %s", net_music_catalog_base_url().c_str());
    } else {
        LOGW("[应用] NAS base 重新加载失败");
    }
    // TF 卡插入后不再自动重连 WiFi。
    // WiFi 总开关由 NVS 保存；如需联网，由用户在菜单中手动开启/重连。
    // 这样可以避免插卡后 WiFi 扫描/连接影响本地播放稳定性。
    LOGD("[应用] TF 卡挂载后已跳过 WiFi 重连");

    if (!storage_catalog_v3_load_or_rebuild("/Music", SystemPaths::kMusicIndexV3)) {
        LOGE("[应用] 目录 re加载 失败 after TF 已挂载");
        ui_show_player_placeholder("曲库加载失败", "请重插TF卡或重扫曲库");
        return;
    }

    player_playlist_reset_state();
    player_playlist_force_rebuild();

    // 关键修复：
    // 网络电台播放中插卡，只加载 TF 资源，不恢复本地歌曲快照。
    if (radio_active) {
        LOGD("[应用] TF 已挂载 while 电台 active: 跳过 本地 snapshot 恢复");
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
        LOGD("[应用] TF 已挂载: snapshot 恢复 armed");
        ui_request_refresh_now();
        return;
    }

    // 无快照属于正常状态：直接预选当前播放列表第一首并进入播放器 UI。
    // 只有曲库、播放列表或歌曲信息真正失败时才显示错误占位页。
    (void)app_prepare_tf_player_ui();
}

/* 根据当前应用状态执行相应的状态处理函数 */
void app_state_update(void)
{
    // 先维护 I2C 总线和 MCP23017。总线异常时先恢复，再扫描扩展按键，
    // 避免多个按键读取连续触发超时并拖慢整个主循环。
    board_hw_i2c_service();

    // 按键处理也需要高频调用，确保响应及时。
    keys_update();
    // 霍尔采用稳定边沿控制；电磁铁联动时还负责到位确认和播放/暂停。
    hall_control_tick();
    // WS2812 根据实际播放状态、音源和顺序/随机模式非阻塞刷新。
    ws2812_status_tick();
    // TC118S 电磁铁只允许短脉冲输出，tick 到时后自动断电。
    board_hw_solenoid_tick();
    web_server_poll();

    // 睡眠关机定时器到点后走统一安全关机流程。
    app_power_sleep_timer_tick();

    if (!s_low_battery_shutdown_started) {
        const BatteryShutdownReason reason = board_hw_battery_shutdown_reason();
        if (reason != BatteryShutdownReason::None) {
            s_low_battery_shutdown_started = true;
            LOGW("[应用] 触发低电量关机: %s", board_hw_battery_shutdown_reason_label(reason));
            // 低电量也走统一保存关机流程：同时保存本地/NAS 双快照、
            // 普通音量、蓝牙发射音量、列表/Web/NFC 脏数据，再安全断电。
            app_power_save_and_shutdown();
            return;
        }
    }

    // 高频主循环只读取轻量快照，避免复制 String，也避免直接读取跨任务扫描标志。
    const PlayerSourceRuntimeState source = player_source_runtime_get();
    const AppRescanState rescan = app_rescan_state_get();

    const bool local_audio_active =
        source.type == PlayerSourceType::LOCAL_TRACK &&
        (audio_service_is_playing() || audio_service_is_paused());

    // 无 CD 脚时，播放中不主动探测，避免干扰正常读卡；
    // 但一旦 AudioFile 上报 IO error，就必须允许热插拔探测抢占，
    // 否则播放器会把“拔卡”误判成“单曲播放失败”并连续 auto next。
    const StorageRuntimeSnapshot storage = storage_runtime_snapshot_get();
    const bool storage_suspect = storage.recent_io_error;
    const bool allow_sd_probe =
        g_app_state != STATE_BOOT &&
        !rescan.rescanning &&
        (!local_audio_active || storage_suspect);

    const StorageHotplugEvent hotplug_event = storage_hotplug_poll(allow_sd_probe);
    if (hotplug_event == StorageHotplugEvent::CARD_REMOVED) {
        app_handle_tf_removed();
    } else if (hotplug_event == StorageHotplugEvent::CARD_MOUNTED) {
        app_handle_tf_mounted();
    }

    if (s_tf_mount_restore_pending) {
        if (player_source_type_get() == PlayerSourceType::NET_RADIO) {
            LOGW("[应用] 取消 本地 snapshot 恢复: 电台 is active");
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
            LOGD("[应用] TF 卡已挂载：快照恢复完成");
            return;
        }

        LOGW("[应用] TF 已挂载: snapshot 恢复失败，回退到播放列表第一首");

        // 快照歌曲不存在等恢复失败属于可恢复状态：进入播放器 UI，但不自动播放。
        (void)app_prepare_tf_player_ui();
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

/* 请求进入 NFC 管理状态并指定目标 */
bool app_request_enter_nfc_admin_with_target(const NfcAdminTarget& target)
{
    if (g_app_state != STATE_PLAYER) {
        LOGD("[应用] 进入 NFC admin 被拒绝: not in player 状态");
        return false;
    }

    if (app_rescan_state_get().rescanning) {
        LOGD("[应用] 拒绝进入 NFC 管理：曲库正在重扫");
        return false;
    }

    if (player_list_select_is_active()) {
        LOGD("[应用] 拒绝进入 NFC 管理：正在列表选择模式");
        return false;
    }

    LOGI("[应用] 进入 NFC 管理：使用指定绑定目标");

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
    LOGI("[应用] 退出ing NFC admin");

    ui_hold_render(true);

    keys_sync_to_hw_state();

    nfc_admin_state_exit();
    g_app_state = STATE_PLAYER;

    ui_return_to_player();

    ui_hold_render(false);

    if (nfc_admin_state_consume_resume_request()) {
        LOGD("[应用] NFC admin 退出: 恢复 当前 歌曲");
        player_toggle_play(PlayerToggleTrigger::NfcAdminResume);
    }
}

// 请求启动扫描
bool app_request_start_rescan(AppRescanMode mode)
{
    if (g_app_state != STATE_PLAYER) {
        LOGD("[应用] 启动 rescan 被拒绝: not in player 状态");
        return false;
    }
    if (app_rescan_state_get().rescanning) {
        LOGD("[应用] 拒绝开始重扫：已有重扫正在进行");
        return false;
    }
    if (player_list_select_is_active()) {
        LOGD("[应用] 拒绝开始重扫：正在列表选择模式");
        return false;
    }
    if (!storage_is_ready()) {
        LOGD("[应用] 启动 rescan 被拒绝: storage 未就绪");
        return false;
    }

    player_recover_prepare_rescan_restore_current();
    if (!app_rescan_begin(mode)) {
        LOGD("[应用] 拒绝开始重扫：状态已被其它任务占用");
        return false;
    }

    BaseType_t ok = xTaskCreate(
        app_rescan_task_entry,
        "rescan_v3",
        8192,
        nullptr,
        1,
        nullptr);

    if (ok != pdPASS) {
        LOGE("[应用] 创建曲库重扫任务失败");
        app_rescan_reset();
        return false;
    }

    const char* mode_label = "快速增量";
    if (mode == AppRescanMode::UltraFastIncremental) {
        mode_label = "超快速目录";
    } else if (mode == AppRescanMode::StrictIncremental) {
        mode_label = "严格增量";
    } else if (mode == AppRescanMode::Full) {
        mode_label = "强制全量";
    }
    LOGI("[应用] 已启动曲库重扫：模式=%s", mode_label);
    return true;
}

// 请求取消扫描
bool app_request_cancel_rescan()
{
    const AppRescanState rescan = app_rescan_state_get();
    if (!rescan.rescanning || rescan.done) {
        LOGD("[应用] 忽略取消重扫请求：当前没有可取消的重扫任务");
        return false;
    }

    if (rescan.abort_requested) {
        return true;
    }

    if (!app_rescan_request_abort()) {
        return false;
    }

    LOGI("[应用] 已请求取消曲库重扫");
    return true;
}
