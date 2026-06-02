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

static void prepare_music_catalogs()
{
    if (storage_catalog_v3_load_or_rebuild("/Music",
                                           "/System/music_index_v3.bin")) {
        LOGI("[BOOT] V3 ready: tracks=%lu albums=%lu artists=%lu",
             (unsigned long)storage_catalog_v3_track_count(),
             (unsigned long)storage_catalog_v3_album_count(),
             (unsigned long)storage_catalog_v3_artist_count());

        storage_catalog_v3_log_memory_stats();
    } else {
        LOGE("[BOOT] V3 load/rebuild failed");
    }
}

void boot_state_run(void)
{
    static bool done = false;
    if (done) return;
    done = true;

    Serial.begin(115200);
    delay(300);
    Serial.println("[BOOT] start");
    
    Serial.printf("[MEM] psramFound=%d, PsramSize=%u, FreePsram=%u\n",
              (int)psramFound(), (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram());
    Serial.printf("[MEM] FreeHeap=%u\n", (unsigned)ESP.getFreeHeap());

    // 1) 初始化两条 SPI：默认SPI=UI，SPI_SD=SD
    board_spi_init();

    storage_init();
    audio_file_prepare_music_root_cache();

    // 加载 NFC 绑定文件
    if (nfc_binding_load("/System/nfc_map.txt")) {
        LOGI("[BOOT] NFC bindings loaded: %d entries", nfc_binding_count());
    } else {
        LOGI("[BOOT] No NFC bindings found");
    }

    // 初始化封面缓冲区（固定大小，避免 PSRAM 碎片）
    if (!cover_init_buffer()) {
        Serial.println("[BOOT] 封面缓冲区初始化失败");
    }

    // 2) 先点亮屏幕  启动 UI（TFT_eSPI 用默认 SPI，不会再打架）
    ui_init();

    // ✅ 启动音频专用任务（双核：音频与UI分离，避免旋转推屏导致卡顿）
    audio_service_start();
    runtime_monitor_start();

    nfc_init();

    // 让用户看到"启动中..."界面
    // delay(1000);

    prepare_music_catalogs();

    // 预加载电台列表到内存中
    #include "radio/radio_catalog.h"
    if (radio_catalog_load()) {
        LOGI("[BOOT] Radio catalog loaded: %d stations", (int)radio_catalog_count());
    } else {
        LOGW("[BOOT] Radio catalog load failed");
    }

    // 提前从 NVS 读取待恢复快照；真正恢复播放在首次进入 player 状态时执行。
    player_snapshot_load_pending_from_nvs();

    // 启动网页控制 MVP（优先连已配置 Wi-Fi，失败则回退到 AP 热点模式）
    web_server_start();

    Serial.println("[BOOT] -> PLAYER");

    g_app_state = STATE_PLAYER;
}
