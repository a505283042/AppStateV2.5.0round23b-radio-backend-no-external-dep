#include "boot_state.h"
#include "app_state.h"
#include "nfc/nfc.h"
#include "nfc/nfc_binding.h"
#include "board/board_spi.h"
#include "storage/storage.h"
#include "storage/storage_catalog_v3.h"
#include "storage/system_paths.h"
#include "ui/ui.h"
#include "ui/ui_cover_mem.h"
#include "utils/log.h"
#include "audio/audio_service.h"
#include "audio/audio_file.h"
#include "utils/runtime_monitor.h"
#include "utils/panic_diag.h"
#include "web/web_server.h"
#include "player_snapshot.h"
#include "app_alarm.h"
#include "net_music/net_music_catalog.h"
#include "keys/keys.h"
#include "hal/mcp23017_u3.h"
#include "player_list_select.h"

static void prepare_music_catalogs()
{
    if (storage_catalog_v3_load_or_rebuild("/Music",
                                        SystemPaths::kMusicIndexV3)) {
        LOGD("[启动] V3歌曲库 加载成功: 音乐=%lu 专辑=%lu 歌手=%lu",
            (unsigned long)storage_catalog_v3_track_count(),
            (unsigned long)storage_catalog_v3_album_count(),
            (unsigned long)storage_catalog_v3_artist_count());

        // 曲库内存归因已在 storage_catalog_v3_load_or_rebuild() 成功后强制打印。
    } else {
        LOGE("[启动] V3歌曲库 加载失败");
    }
}

void boot_state_run(void)
{
    static bool done = false;
    if (done) return;

    // 主串口已经在 setup() 中初始化，这里只输出启动阶段日志。
    Serial.println("[启动] 开始");

    Serial.printf("[内存] PSRAM存在=%d，总容量=%u，可用=%u\n",
                (int)psramFound(), (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram());
    Serial.printf("[内存] 内部堆可用=%u\n", (unsigned)ESP.getFreeHeap());

    // 1) 初始化两条 SPI：默认SPI=UI，SPI_SD=SD。
    // UI 与 NFC 共享 SPI，互斥量创建失败时不能继续启动；保留 BOOT 状态供下一轮重试。
    if (!board_spi_init()) {
        LOGE("[启动] SPI 总线或共享锁初始化失败，稍后重试");
        return;
    }
    done = true;

    // keys_init() 早于 MCP23017 初始化执行。扩展器就绪后必须重新同步一次，
    // 消费上电期间的残留电平，避免首次松键被误判为短按。
    if (mcp23017_u3_is_ready()) {
        keys_sync_to_hw_state();
        LOGI("[启动] MCP23017 就绪后已重新同步按键状态");
    } else {
        LOGW("[启动] MCP23017 未就绪，扩展按键保持未按下状态");
    }

    // RTC 初始化完成后加载收音机闹钟配置。
    // 如果闹钟已启用，这里会把正式闹钟重新写入 PCF85063A。
    app_alarm_begin();

    const bool sd_ok = storage_init();
    if (sd_ok) {
        // 先创建分类目录并迁移旧版 /System 根目录文件，后续模块统一使用新路径。
        (void)storage_system_layout_prepare();

        // 上一次如果是 Guru Meditation / WDT / panic，ESP-IDF 会先把 core dump 写入 flash。
        // 这里等 TF 卡和 SdFat 已经稳定挂载后，再复制到 /System/crash/coredump_xxxxxxxx.bin。
        panic_diag_flush_to_sd();

        audio_file_prepare_music_root_cache();

        // 加载 NFC 绑定文件
        if (nfc_binding_load(SystemPaths::kNfcMap)) {
            LOGD("[启动] NFC 绑定表 加载成功: %d 条", nfc_binding_count());
        } else {
            LOGI("[启动] 未找到 NFC 绑定表");
        }
    } else {
        LOGW("[启动] 没有 TF 卡，不加载本地库");
        nfc_binding_clear();
        storage_catalog_v3_clear();
    }

    // 初始化封面缓冲区（固定大小，避免 PSRAM 碎片）
    if (!cover_init_buffer()) {
        Serial.println("[启动] 封面缓冲区初始化失败");
    }

    // UiTask 启动前先建立列表可见页快照互斥量，避免 UI 首帧并发读取未初始化状态。
    player_list_select_init();

    // 2) 先点亮屏幕  启动 UI（TFT_eSPI 用默认 SPI，不会再打架）
    ui_init();

    // ✅ 启动音频专用任务（双核：音频与UI分离，避免旋转推屏导致卡顿）
    audio_service_start();
    if (!runtime_monitor_start()) {
        // 监控任务只负责诊断，创建失败不阻止播放器进入主功能。
        LOGW("[启动] RuntimeMon 创建失败，继续启动播放器");
    }

    nfc_init();

    // 让用户看到"启动中..."界面
    // delay(1000);

    if (storage_is_ready()) {
        prepare_music_catalogs();
    } else {
        LOGW("[启动] 本地库加载失败，存储未就绪");
    }

    // 预加载电台列表到内存中
    #include "radio/radio_catalog.h"
    if (storage_is_ready() && radio_catalog_load()) {
        LOGD("[启动] 电台列表 加载成功: %d 个电台", (int)radio_catalog_count());
    } else {
        LOGW("[启动] 加载电台列表失败");    
    }

    // NAS/HTTP 歌曲索引不在开机阶段预加载。
    // 开机只读取很小的 /System/config/net_music_base.txt。
    // 打开 NAS 歌曲列表或 Web NAS 页面时，再从 base URL 下载 net_music.txt 到内存。
    // 注意：不会把 NAS 歌曲列表写入 TF 卡，避免和本地播放抢卡。
    if (storage_is_ready() && net_music_catalog_load_base()) {
        LOGD("[启动] NAS base 加载成功: %s", net_music_catalog_base_url().c_str());
    } else {
        LOGW("[启动] NAS base 未加载");
    }

    // 提前从 NVS 读取待恢复快照；真正恢复播放在首次进入 player 状态时执行。
    // 注意：只有存储就绪时才读取 snapshot，因为 snapshot key 依赖卡身份。
    if (storage_is_ready()) {
        player_snapshot_load_pending_from_nvs();
    }

    Serial.println("[启动] 初始化完成，进入播放器");
    g_app_state = STATE_PLAYER;

    // Web/WiFi 异步启动，不阻塞进入播放器。
    web_server_start_async();
}