#include "boot_state.h"
#include "app_state.h"
#include "nfc/nfc.h"
#include "nfc/nfc_binding.h"
#include "board/board_spi.h"
#include "storage/storage.h"
#include "storage/storage_catalog_v3.h"
#include "ui/ui.h"
#include "ui/ui_cover_mem.h"
#include "utils/log.h"
#include "audio/audio_service.h"
#include "audio/audio_file.h"
#include "utils/runtime_monitor.h"
#include "web/web_server.h"
#include "player_snapshot.h"
#include "net_music/net_music_catalog.h"

static void prepare_music_catalogs()
{
    if (storage_catalog_v3_load_or_rebuild("/Music",
                                           "/System/music_index_v3.bin")) {
        LOGD("[启动] V3歌曲库 加载成功: 音乐=%lu 专辑=%lu 歌手=%lu",
             (unsigned long)storage_catalog_v3_track_count(),
             (unsigned long)storage_catalog_v3_album_count(),
             (unsigned long)storage_catalog_v3_artist_count());

        // 曲库内存明细有 8~9 行，日常开机只需要上面的汇总。
        // 需要排查内存时，把 platformio.ini 里的 LOG_LEVEL 调到 3 再显示。
        #if LOG_LEVEL >= 3
        storage_catalog_v3_log_memory_stats();
        #endif
    } else {
        LOGE("[启动] V3歌曲库 加载失败");
    }
}

void boot_state_run(void)
{
    static bool done = false;
    if (done) return;
    done = true;

    Serial.begin(115200);
    delay(300);
    Serial.println("[启动] 开始");

    Serial.printf("[内存] PSRAM存在=%d，总容量=%u，可用=%u\n",
                (int)psramFound(), (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram());
    Serial.printf("[内存] 内部堆可用=%u\n", (unsigned)ESP.getFreeHeap());

    // 1) 初始化两条 SPI：默认SPI=UI，SPI_SD=SD
    board_spi_init();

    const bool sd_ok = storage_init();
    if (sd_ok) {
        audio_file_prepare_music_root_cache();

        // 加载 NFC 绑定文件
        if (nfc_binding_load("/System/nfc_map.txt")) {
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

    // 2) 先点亮屏幕  启动 UI（TFT_eSPI 用默认 SPI，不会再打架）
    ui_init();

    // ✅ 启动音频专用任务（双核：音频与UI分离，避免旋转推屏导致卡顿）
    audio_service_start();
    runtime_monitor_start();

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
    // 开机只读取很小的 /System/net_music_base.txt。
    // 打开 NAS 歌曲列表或 Web NAS 页面时，再从 base URL 下载 net_music.txt 到内存。
    // 注意：不会读取/写入 /System/net_music.txt，避免和本地播放抢 TF 卡。
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