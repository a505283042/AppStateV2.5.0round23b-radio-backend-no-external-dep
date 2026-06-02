#include "storage/storage.h"
#include "board/board_pins.h"
#include "board/board_spi.h"
#include "storage/storage_io.h"

#include <Arduino.h>
#include <FS.h>
#include <SdFat.h>

// 全局 SD 文件系统对象（SdFat = FAT16/32，足够用）
SdFat sd;
static bool storage_ready = false;


bool storage_init(void)
{
    Serial.println("[STORAGE] init (SdFat)");

    // 初始化 SD 卡访问互斥锁，在 sd.begin() 之前
    if (!storage_sd_init_mutex()) {
        Serial.println("[STORAGE] 创建 SD 互斥锁失败");
        return false;
    }

    pinMode(PIN_SD_CS, OUTPUT);
    digitalWrite(PIN_SD_CS, HIGH);
    
    // 用独立 SPI_SD 初始化 SdFat
    // DEDICATED_SPI：SdFat 会更积极地管理 SPI（更稳）
    // 提高时钟频率至 24MHz 以提升性能，特别是在解析 FLAC 封面图时
    SdSpiConfig cfg(PIN_SD_CS, DEDICATED_SPI, SD_SCK_MHZ(24), &SPI_SD);

    // 检查是否已经挂载，如果是则先卸载
    if (storage_ready) {
        Serial.println("[STORAGE] 检测到已有挂载，尝试重新挂载");
        sd.end();
        storage_ready = false;
    }

    // 检查卡的错误状态
    if (sd.card()) {
        uint8_t err = sd.card()->errorCode();
        if (err != 0) {
            Serial.printf("[STORAGE] 卡错误代码: %d\n", err);
        }
    }

    if (!sd.begin(cfg)) {
        Serial.println("[STORAGE] SdFat mount FAILED");
        storage_ready = false;
        return false;
    }

    Serial.println("[STORAGE] SdFat mount OK");
    storage_ready = true;

    storage_list_root();
    return true;
}



bool storage_is_ready(void)
{
    return storage_ready;
}

void storage_list_root(void)
{
    if (!storage_ready) return;

    StorageSdLockGuard sd_lock(1000);
    if (!sd_lock) {
        Serial.println("[STORAGE] 无法获取 SD 互斥锁");
        return;
    }

    SdFile root;
    if (!root.open("/")) {
        Serial.println("[STORAGE] open / FAILED");
        return;
    }

    Serial.println("[STORAGE] list /");
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
    root.close();

}
