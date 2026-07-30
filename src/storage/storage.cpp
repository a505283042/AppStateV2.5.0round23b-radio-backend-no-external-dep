#include "storage/storage.h"
#include "board/board_pins.h"
#include "board/board_spi.h"
#include "storage/storage_io.h"
#include "utils/log.h"

#include <Arduino.h>
#include <FS.h>
#include <SdFat.h>
#include <SPI.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <stdio.h>
#include <string.h>

// 全局 SD 文件系统对象（SdFat = FAT16/32，足够用）
SdFat sd;

namespace {

struct StorageRuntimeState {
    bool ready = false;
    bool recent_io_error = false;
    uint32_t io_error_generation = 0;
    uint32_t card_hash = 0;
    char card_snapshot_key[16] = "snap_default";
    uint32_t revision = 1;
};

StorageRuntimeState s_runtime{};
portMUX_TYPE s_runtime_mux = portMUX_INITIALIZER_UNLOCKED;

void storage_revision_advance_locked()
{
    ++s_runtime.revision;
    if (s_runtime.revision == 0) {
        ++s_runtime.revision;
    }
}

void storage_identity_reset_locked()
{
    s_runtime.card_hash = 0;
    strncpy(s_runtime.card_snapshot_key,
            "snap_default",
            sizeof(s_runtime.card_snapshot_key) - 1);
    s_runtime.card_snapshot_key[sizeof(s_runtime.card_snapshot_key) - 1] = '\0';
}

void storage_publish_not_ready(bool clear_io_error)
{
    portENTER_CRITICAL(&s_runtime_mux);
    s_runtime.ready = false;
    if (clear_io_error) {
        s_runtime.recent_io_error = false;
    }
    storage_identity_reset_locked();
    storage_revision_advance_locked();
    portEXIT_CRITICAL(&s_runtime_mux);
}

void storage_publish_ready(uint32_t card_hash, const char* snapshot_key)
{
    portENTER_CRITICAL(&s_runtime_mux);
    s_runtime.ready = true;
    s_runtime.recent_io_error = false;
    s_runtime.card_hash = card_hash;
    strncpy(s_runtime.card_snapshot_key,
            (snapshot_key && *snapshot_key) ? snapshot_key : "snap_default",
            sizeof(s_runtime.card_snapshot_key) - 1);
    s_runtime.card_snapshot_key[sizeof(s_runtime.card_snapshot_key) - 1] = '\0';
    storage_revision_advance_locked();
    portEXIT_CRITICAL(&s_runtime_mux);
}

// FNV-1a 32-bit hash
uint32_t fnv1a32(const uint8_t* data, size_t len)
{
    uint32_t hash = 2166136261UL;

    for (size_t i = 0; i < len; ++i) {
        hash ^= data[i];
        hash *= 16777619UL;
    }

    return hash;
}

void storage_read_card_identity_locked(uint32_t& card_hash,
                                       char* snapshot_key,
                                       size_t snapshot_key_size)
{
    card_hash = 0;
    if (snapshot_key && snapshot_key_size > 0) {
        snprintf(snapshot_key, snapshot_key_size, "snap_default");
    }

    if (!sd.card()) {
        Serial.println("[存储] 跳过卡身份读取：无卡对象");
        return;
    }

    cid_t cid;
    memset(&cid, 0, sizeof(cid));

    if (!sd.card()->readCID(&cid)) {
        Serial.printf("[存储] 读取 CID 失败 错误=%u data=%u\n",
                      sd.card()->errorCode(),
                      sd.card()->errorData());
        return;
    }

    card_hash = fnv1a32(reinterpret_cast<const uint8_t*>(&cid), sizeof(cid));

    if (snapshot_key && snapshot_key_size > 0) {
        snprintf(snapshot_key,
                 snapshot_key_size,
                 "snap_%08lX",
                 static_cast<unsigned long>(card_hash));
    }

    Serial.printf("[存储] TF 卡标识=%08lX，快照键=%s\n",
                  static_cast<unsigned long>(card_hash),
                  snapshot_key ? snapshot_key : "snap_default");
}

} // namespace

StorageRuntimeSnapshot storage_runtime_snapshot_get(void)
{
    StorageRuntimeSnapshot snapshot{};

    portENTER_CRITICAL(&s_runtime_mux);
    snapshot.ready = s_runtime.ready;
    snapshot.recent_io_error = s_runtime.recent_io_error;
    snapshot.io_error_generation = s_runtime.io_error_generation;
    snapshot.card_hash = s_runtime.card_hash;
    memcpy(snapshot.card_snapshot_key,
           s_runtime.card_snapshot_key,
           sizeof(snapshot.card_snapshot_key));
    snapshot.card_snapshot_key[sizeof(snapshot.card_snapshot_key) - 1] = '\0';
    snapshot.revision = s_runtime.revision;
    portEXIT_CRITICAL(&s_runtime_mux);

    return snapshot;
}

uint32_t storage_card_hash(void)
{
    return storage_runtime_snapshot_get().card_hash;
}

bool storage_copy_card_snapshot_key(char* out, size_t out_size)
{
    if (!out || out_size == 0) {
        return false;
    }

    const StorageRuntimeSnapshot snapshot = storage_runtime_snapshot_get();
    snprintf(out, out_size, "%s", snapshot.card_snapshot_key);
    return true;
}

bool storage_mount(void)
{
    Serial.println("[存储] 挂载 TF 卡（SdFat）");

    // 初始化 SD 卡访问互斥锁，在 sd.begin() 之前。
    if (!storage_sd_init_mutex()) {
        Serial.println("[存储] 创建 SD 互斥锁失败");
        storage_publish_not_ready(true);
        return false;
    }

    StorageSdLockGuard sd_lock(2000);
    if (!sd_lock) {
        Serial.println("[存储] 挂载失败：等待 SD 锁超时");
        storage_publish_not_ready(true);
        return false;
    }

    pinMode(PIN_SD_CS, OUTPUT);
    digitalWrite(PIN_SD_CS, HIGH);

    // sd.end() 或拔卡异常后，Arduino SPIClass 可能被 SdFat 释放/复位。
    // ESP32-S3 的 HSPI 没有默认引脚，重新 mount 前必须显式 begin 自定义 SD 引脚。
    SPI_SD.begin(PIN_SPI_SD_SCK, PIN_SPI_SD_MISO, PIN_SPI_SD_MOSI, PIN_SD_CS);

    // 注意：热插拔 + 多任务访问 SdFat 时不要使用 DEDICATED_SPI。
    // DEDICATED_SPI 可能把多块读取事务留到下一次 SD 操作再收尾，
    // 如果下一次操作发生在另一个任务，会触发 Arduino SPI mutex 断言。
    // SHARED_SPI 会让每次操作在当前任务内成对 begin/end transaction，更适合当前工程。
    SdSpiConfig cfg(PIN_SD_CS, SHARED_SPI, SD_SCK_MHZ(24), &SPI_SD);

    const StorageRuntimeSnapshot before_mount = storage_runtime_snapshot_get();
    if (before_mount.ready) {
        Serial.println("[存储] 检测到已有挂载，尝试重新挂载");
        storage_publish_not_ready(true);
        sd.end();
    }

    // 检查卡的错误状态。
    if (sd.card()) {
        const uint8_t err = sd.card()->errorCode();
        if (err != 0) {
            Serial.printf("[存储] 卡错误代码：%d\n", err);
        }
    }

    if (!sd.begin(cfg)) {
        Serial.println("[存储] TF 卡挂载失败");
        storage_publish_not_ready(true);
        digitalWrite(PIN_SD_CS, HIGH);
        return false;
    }

    uint32_t card_hash = 0;
    char snapshot_key[16] = "snap_default";
    storage_read_card_identity_locked(card_hash,
                                      snapshot_key,
                                      sizeof(snapshot_key));

    // 卡身份、就绪状态和 IO 错误状态一次性发布，调用方不会读到新卡配旧身份。
    storage_publish_ready(card_hash, snapshot_key);

    Serial.println("[存储] TF 卡挂载成功");

#if LOG_LEVEL >= 3
    // 根目录枚举只用于调试；INFO 构建不再额外打开根目录。
    storage_list_root();
#endif
    return true;
}

bool storage_init(void)
{
    return storage_mount();
}

void storage_mark_not_ready(void)
{
    // 拔卡确认后立即阻止新文件访问，并同步清除旧卡身份。
    // 最近 IO 错误保留到真正 unmount 完成，便于故障路径继续识别本次拔卡。
    storage_publish_not_ready(false);
}

bool storage_unmount(void)
{
    storage_mark_not_ready();

    StorageSdLockGuard sd_lock(2000);
    if (!sd_lock) {
        Serial.println("[存储] 卸载失败：等待 SD 锁超时");
        return false;
    }

    sd.end();
    storage_publish_not_ready(true);

    pinMode(PIN_SD_CS, OUTPUT);
    digitalWrite(PIN_SD_CS, HIGH);

    Serial.println("[存储] TF 卡已卸载");
    return true;
}

bool storage_is_ready(void)
{
    return storage_runtime_snapshot_get().ready;
}

bool storage_probe_alive(void)
{
    if (!storage_runtime_snapshot_get().ready) {
        return false;
    }

    StorageSdLockGuard sd_lock(80);
    if (!sd_lock) {
        // 锁超时不代表 TF 卡异常，可能只是音频/扫描正在访问 SD。
        return true;
    }

    // 等待 SD 锁期间可能已确认拔卡；此时不要再触碰 SdFat 卡对象。
    if (!storage_runtime_snapshot_get().ready) {
        return false;
    }

    if (!sd.card()) {
        Serial.println("[存储] 探测失败：无卡对象");
        return false;
    }

    // 强制读物理 0 扇区。无 CD 脚时这是比 root.open("/") 更可靠的存在性探测。
    // 探测缓冲只在本次调用期间使用，放入任务栈，避免 512B 长期占用内部 BSS。
    uint8_t probe_sector[512];
    const bool ok = sd.card()->readSector(0, probe_sector);
    if (!ok) {
        Serial.printf("[存储] 探测读扇区失败 错误=%u data=%u\n",
                      sd.card()->errorCode(),
                      sd.card()->errorData());
    }

    return ok;
}

void storage_report_io_error(const char* where)
{
    uint32_t generation = 0;

    portENTER_CRITICAL(&s_runtime_mux);
    s_runtime.recent_io_error = true;
    ++s_runtime.io_error_generation;
    if (s_runtime.io_error_generation == 0) {
        ++s_runtime.io_error_generation;
    }
    generation = s_runtime.io_error_generation;
    storage_revision_advance_locked();
    portEXIT_CRITICAL(&s_runtime_mux);

    Serial.printf("[存储] 记录 IO 错误：%s 代次=%lu\n",
                  where ? where : "(未知)",
                  static_cast<unsigned long>(generation));
}

bool storage_has_recent_io_error(void)
{
    return storage_runtime_snapshot_get().recent_io_error;
}

bool storage_clear_io_error_if_generation(uint32_t observed_generation)
{
    bool cleared = false;

    portENTER_CRITICAL(&s_runtime_mux);
    if (s_runtime.recent_io_error &&
        s_runtime.io_error_generation == observed_generation) {
        s_runtime.recent_io_error = false;
        storage_revision_advance_locked();
        cleared = true;
    }
    portEXIT_CRITICAL(&s_runtime_mux);

    return cleared;
}

void storage_list_root(void)
{
    if (!storage_runtime_snapshot_get().ready) {
        return;
    }

    StorageSdLockGuard sd_lock(1000);
    if (!sd_lock) {
        Serial.println("[存储] 无法获取 SD 互斥锁");
        return;
    }

    if (!storage_runtime_snapshot_get().ready) {
        return;
    }

    SdFile root;
    if (!root.open("/")) {
        Serial.println("[存储] 打开根目录失败");
        return;
    }

#if LOG_LEVEL >= 3
    // 根目录列表属于启动排查日志，日常 INFO 启动不打印。
    Serial.println("[存储] 根目录列表：");
    SdFile f;
    while (f.openNext(&root, O_RDONLY)) {
        char name[128];
        f.getName(name, sizeof(name));

        if (f.isDir()) {
            Serial.printf("  %s <DIR>\n", name);
        } else {
            Serial.printf("  %s  %lu 字节\n", name, (unsigned long)f.fileSize());
        }
        f.close();
    }
#endif
    root.close();
}
