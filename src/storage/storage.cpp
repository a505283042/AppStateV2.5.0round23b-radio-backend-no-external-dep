#include "storage/storage.h"
#include "board/board_pins.h"
#include "board/board_spi.h"
#include "storage/storage_io.h"
#include "utils/log.h"

#include <Arduino.h>
#include <FS.h>
#include <SdFat.h>
#include <SPI.h>

// 全局 SD 文件系统对象（SdFat = FAT16/32，足够用）
SdFat sd;
static volatile bool storage_ready = false;
static volatile bool s_recent_io_error = false;

// TF 卡身份标识
static uint32_t s_card_hash = 0;
static char s_card_snapshot_key[16] = "snap_default";

// FNV-1a 32-bit hash
static uint32_t fnv1a32(const uint8_t* data, size_t len)
{
    uint32_t hash = 2166136261UL;

    for (size_t i = 0; i < len; ++i) {
        hash ^= data[i];
        hash *= 16777619UL;
    }

    return hash;
}

// 轻量探测用缓冲区。读 sector 0 可以强制触碰物理卡，
// 比 root.open("/") 更可靠，避免目录对象缓存导致误判"卡还在"。
static uint8_t s_probe_sector[512];

static void storage_refresh_card_identity_locked()
{
    s_card_hash = 0;
    snprintf(s_card_snapshot_key,
             sizeof(s_card_snapshot_key),
             "snap_default");

    if (!sd.card()) {
        Serial.println("[存储] 卡标识跳过：无卡");  
        return;
    }

    cid_t cid;
    memset(&cid, 0, sizeof(cid));

    if (!sd.card()->readCID(&cid)) {
        Serial.printf("[存储] 读取 TF 卡标识失败，错误码=%u，数据=%u\n",
                      sd.card()->errorCode(),
                      sd.card()->errorData());
        return;
    }

    s_card_hash = fnv1a32(reinterpret_cast<const uint8_t*>(&cid), sizeof(cid));

    snprintf(s_card_snapshot_key,
             sizeof(s_card_snapshot_key),
             "snap_%08lX",
             static_cast<unsigned long>(s_card_hash));

    Serial.printf("[存储] TF 卡标识=%08lX，快照键=%s\n",
                  static_cast<unsigned long>(s_card_hash),
                  s_card_snapshot_key);
}

uint32_t storage_card_hash(void)
{
    return s_card_hash;
}

const char* storage_card_snapshot_key(void)
{
    return s_card_snapshot_key;
}

void storage_clear_card_identity(void)
{
    s_card_hash = 0;
    snprintf(s_card_snapshot_key,
             sizeof(s_card_snapshot_key),
             "snap_default");
}

bool storage_mount(void)
{
    Serial.println("[存储] 挂载 TF 卡（SdFat）");

    // 初始化 SD 卡访问互斥锁，在 sd.begin() 之前
    if (!storage_sd_init_mutex()) {
        Serial.println("[STORAGE] 创建 SD 互斥锁失败");
        storage_ready = false;
        return false;
    }

    StorageSdLockGuard sd_lock(2000);
    if (!sd_lock) {
        Serial.println("[存储] 挂载锁超时");
        storage_ready = false;
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

    // 检查是否已经挂载，如果是则先卸载
    if (storage_ready) {
        Serial.println("[存储] 检测到已有挂载，尝试重新挂载");
        sd.end();
        storage_ready = false;
    }

    // 检查卡的错误状态
    if (sd.card()) {
        uint8_t err = sd.card()->errorCode();
        if (err != 0) {
            Serial.printf("[存储] 卡错误代码: %d\n", err);
        }
    }

    if (!sd.begin(cfg)) {
        Serial.println("[存储] TF 卡挂载失败");
        storage_ready = false;
        digitalWrite(PIN_SD_CS, HIGH);
        return false;
    }

    Serial.println("[存储] TF 卡挂载成功");
    storage_ready = true;
    s_recent_io_error = false;

    storage_refresh_card_identity_locked();

    storage_list_root();
    return true;
}

bool storage_init(void)
{
    return storage_mount();
}

void storage_mark_not_ready(void)
{
    storage_ready = false;
}

bool storage_unmount(void)
{
    storage_mark_not_ready();

    StorageSdLockGuard sd_lock(2000);
    if (!sd_lock) {
        Serial.println("[存储] 卸载锁超时");
        return false;
    }

    sd.end();
    storage_clear_card_identity();

    pinMode(PIN_SD_CS, OUTPUT);
    digitalWrite(PIN_SD_CS, HIGH);
    Serial.println("[存储] TF 卡卸载完成");
    return true;
}



bool storage_is_ready(void)
{
    return storage_ready;
}

bool storage_probe_alive(void)
{
    if (!storage_ready) {
        return false;
    }

    StorageSdLockGuard sd_lock(80);
    if (!sd_lock) {
        // 锁超时不代表 TF 卡异常，可能只是音频/扫描正在访问 SD。
        return true;
    }

    if (!sd.card()) {
        Serial.println("[存储] 无卡对象");
        return false;
    }

    // 强制读物理 0 扇区。无 CD 脚时这是比 root.open("/") 更可靠的存在性探测。
    const bool ok = sd.card()->readSector(0, s_probe_sector);
    if (!ok) {
        Serial.printf("[存储] 读取扇区错误: %d, 数据: %d\n",
                      sd.card()->errorCode(),
                      sd.card()->errorData());
    }

    return ok;
}

void storage_report_io_error(const char* where)
{
    s_recent_io_error = true;
    Serial.printf("[存储] IO 错误报告: %s\n", where ? where : "(未知)");
}

bool storage_has_recent_io_error(void)
{
    return s_recent_io_error;
}

void storage_clear_io_error(void)
{
    s_recent_io_error = false;
}

void storage_list_root(void)
{
    if (!storage_ready) return;

    StorageSdLockGuard sd_lock(1000);
    if (!sd_lock) {
        Serial.println("[存储] 无法获取 SD 互斥锁");
        return;
    }

    SdFile root;
    if (!root.open("/")) {
        Serial.println("[存储] 打开根目录失败");
        return;
    }

    #if LOG_LEVEL >= 3
        // 根目录列表属于启动排查日志，日常 INFO 启动不打印。
        Serial.println("[存储] 根目录列表");
        SdFile f;
        while (f.openNext(&root, O_RDONLY)) {
            char name[128];
            f.getName(name, sizeof(name));

            if (f.isDir()) {
                Serial.printf("  %s <DIR>\n", name);
            } else {
                Serial.printf("  %s  %lu bytes\n", name, (unsigned long)f.fileSize());
            }
            f.close();
        }
    #endif
    
    root.close();

}