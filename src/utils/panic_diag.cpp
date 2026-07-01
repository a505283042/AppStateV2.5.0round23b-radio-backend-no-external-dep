#include "utils/panic_diag.h"

#include <Arduino.h>
#include <SdFat.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "storage/storage.h"
#include "storage/storage_io.h"
#include "utils/log.h"

#include "esp_err.h"
#include "esp_idf_version.h"
#include "esp_partition.h"
#include "esp_system.h"

#if __has_include("esp_core_dump.h")
  #include "esp_core_dump.h"
  #define PANIC_DIAG_HAS_CORE_DUMP_HEADER 1
#else
  #define PANIC_DIAG_HAS_CORE_DUMP_HEADER 0
#endif

#if PANIC_DIAG_HAS_CORE_DUMP_HEADER && \
    ((defined(CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH) && CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH) || \
     (defined(CONFIG_ESP32_ENABLE_COREDUMP_TO_FLASH) && CONFIG_ESP32_ENABLE_COREDUMP_TO_FLASH))
  #define PANIC_DIAG_CORE_DUMP_TO_FLASH 1
#else
  #define PANIC_DIAG_CORE_DUMP_TO_FLASH 0
#endif

extern SdFat sd;

static void file_printf(File32& f, const char* fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    f.print(buf);
}

const char* panic_diag_reset_reason_name(int reason)
{
    switch ((esp_reset_reason_t)reason) {
        case ESP_RST_UNKNOWN:   return "ESP_RST_UNKNOWN";
        case ESP_RST_POWERON:   return "ESP_RST_POWERON";
        case ESP_RST_EXT:       return "ESP_RST_EXT";
        case ESP_RST_SW:        return "ESP_RST_SW";
        case ESP_RST_PANIC:     return "ESP_RST_PANIC";
        case ESP_RST_INT_WDT:   return "ESP_RST_INT_WDT";
        case ESP_RST_TASK_WDT:  return "ESP_RST_TASK_WDT";
        case ESP_RST_WDT:       return "ESP_RST_WDT";
        case ESP_RST_DEEPSLEEP: return "ESP_RST_DEEPSLEEP";
        case ESP_RST_BROWNOUT:  return "ESP_RST_BROWNOUT";
        case ESP_RST_SDIO:      return "ESP_RST_SDIO";
        default:                return "ESP_RST_OTHER";
    }
}

static bool is_abnormal_reset(esp_reset_reason_t reason)
{
    return reason == ESP_RST_PANIC ||
           reason == ESP_RST_INT_WDT ||
           reason == ESP_RST_TASK_WDT ||
           reason == ESP_RST_WDT ||
           reason == ESP_RST_BROWNOUT;
}

static uint32_t next_panic_sequence()
{
    // 避免引入 Arduino Preferences 额外依赖，使用 NVS 底层 API 会更长。
    // 这里用 reset 后仍可区分的随机数作为文件后缀；文件名不要求连续，只要求不易覆盖。
    uint32_t v = esp_random();
    if (v == 0) v = (uint32_t)millis() ^ 0xA5A50001UL;
    return v;
}

static void write_summary(File32& f,
                          esp_reset_reason_t reset_reason,
                          bool abnormal,
                          bool coredump_enabled,
                          esp_err_t coredump_check,
                          size_t coredump_addr,
                          size_t coredump_size,
                          const char* coredump_file,
                          bool copied)
{
    file_printf(f, "========== panic/reset summary ==========" "\n");
    file_printf(f, "reset_reason=%s(%d)\n",
                panic_diag_reset_reason_name((int)reset_reason),
                (int)reset_reason);
    file_printf(f, "abnormal_reset=%s\n", abnormal ? "yes" : "no");
    file_printf(f, "sdk=%s\n", ESP.getSdkVersion());
    file_printf(f, "chip=%s rev=%u cores=%u\n",
                ESP.getChipModel(),
                (unsigned)ESP.getChipRevision(),
                (unsigned)ESP.getChipCores());
    file_printf(f, "free_heap=%u min_free_heap=%u\n",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMinFreeHeap());
    file_printf(f, "psram_found=%d free_psram=%u\n",
                (int)psramFound(),
                (unsigned)ESP.getFreePsram());
    file_printf(f, "sketch_size=%u sketch_md5=%s\n",
                (unsigned)ESP.getSketchSize(),
                ESP.getSketchMD5().c_str());
    file_printf(f, "coredump_to_flash_enabled=%s\n", coredump_enabled ? "yes" : "no");
    file_printf(f, "coredump_check=%s(%d)\n", esp_err_to_name(coredump_check), (int)coredump_check);
    file_printf(f, "coredump_flash_addr=0x%08x\n", (unsigned)coredump_addr);
    file_printf(f, "coredump_size=%u\n", (unsigned)coredump_size);
    file_printf(f, "coredump_saved=%s\n", copied ? "yes" : "no");
    if (coredump_file && coredump_file[0]) {
        file_printf(f, "coredump_file=%s\n", coredump_file);
    }

#if PANIC_DIAG_CORE_DUMP_TO_FLASH && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    char panic_reason[256] = {0};
    if (esp_core_dump_get_panic_reason(panic_reason, sizeof(panic_reason)) == ESP_OK) {
        file_printf(f, "panic_reason=%s\n", panic_reason);
    }
#endif

    file_printf(f, "note=Use firmware.elf + esp-coredump/idf.py to decode coredump binary.\n");
    file_printf(f, "========== end ==========" "\n\n");
}

#if PANIC_DIAG_CORE_DUMP_TO_FLASH
static bool copy_coredump_partition_to_file(const char* path, size_t image_size)
{
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_COREDUMP,
        nullptr);

    if (!part) {
        LOGE("[崩溃诊断] 未找到 coredump 分区");
        return false;
    }

    if (image_size == 0 || image_size > part->size) {
        LOGE("[崩溃诊断] coredump 大小异常: %u partition=%u",
             (unsigned)image_size,
             (unsigned)part->size);
        return false;
    }

    File32 out = sd.open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (!out) {
        LOGE("[崩溃诊断] 打开 coredump 输出文件失败: %s", path);
        return false;
    }

    const size_t chunk_size = 1024;
    uint8_t* buf = (uint8_t*)malloc(chunk_size);
    if (!buf) {
        out.close();
        LOGE("[崩溃诊断] 分配复制缓冲区失败");
        return false;
    }

    size_t offset = 0;
    bool ok = true;
    while (offset < image_size) {
        const size_t n = min(chunk_size, image_size - offset);
        esp_err_t err = esp_partition_read(part, offset, buf, n);
        if (err != ESP_OK) {
            LOGE("[崩溃诊断] 读取 coredump flash 失败 offset=%u err=%s",
                 (unsigned)offset,
                 esp_err_to_name(err));
            ok = false;
            break;
        }

        const size_t written = out.write(buf, n);
        if (written != n) {
            LOGE("[崩溃诊断] 写入 coredump 文件失败 offset=%u written=%u need=%u",
                 (unsigned)offset,
                 (unsigned)written,
                 (unsigned)n);
            ok = false;
            break;
        }
        offset += n;
    }

    out.flush();
    out.close();
    free(buf);

    if (!ok) {
        sd.remove(path);
    }
    return ok;
}
#endif

bool panic_diag_flush_to_sd(void)
{
    if (!storage_is_ready()) {
        return false;
    }

    StorageSdLockGuard sd_lock(3000);
    if (!sd_lock) {
        LOGW("[崩溃诊断] 等待 SD 锁超时");
        return false;
    }

    sd.mkdir("/System");

    const esp_reset_reason_t reset_reason = esp_reset_reason();
    const bool abnormal = is_abnormal_reset(reset_reason);

    esp_err_t coredump_check = ESP_ERR_NOT_SUPPORTED;
    size_t coredump_addr = 0;
    size_t coredump_size = 0;
    bool copied = false;
    char coredump_path[64] = {0};
    char panic_path[64] = {0};

#if PANIC_DIAG_CORE_DUMP_TO_FLASH
    coredump_check = esp_core_dump_image_check();
    if (coredump_check == ESP_OK) {
        esp_err_t get_err = esp_core_dump_image_get(&coredump_addr, &coredump_size);
        if (get_err != ESP_OK) {
            LOGE("[崩溃诊断] 获取 coredump 地址/大小失败: %s", esp_err_to_name(get_err));
            coredump_check = get_err;
        }
    }
#endif

    // 正常上电/正常软件重启且没有 coredump 时，不刷文件，避免每次开机都写 TF。
    if (!abnormal && coredump_check != ESP_OK) {
        return false;
    }

    const uint32_t seq = next_panic_sequence();
    snprintf(coredump_path, sizeof(coredump_path), "/System/coredump_%08lX.bin", (unsigned long)seq);
    snprintf(panic_path, sizeof(panic_path), "/System/panic_%08lX.txt", (unsigned long)seq);

#if PANIC_DIAG_CORE_DUMP_TO_FLASH
    if (coredump_check == ESP_OK && coredump_size > 0) {
        copied = copy_coredump_partition_to_file(coredump_path, coredump_size);
        if (copied) {
            LOGI("[崩溃诊断] 已保存 coredump: %s size=%u",
                 coredump_path,
                 (unsigned)coredump_size);
            // 成功复制后擦除 flash 里的旧 coredump，避免每次开机重复保存同一个崩溃。
            esp_err_t erase_err = esp_core_dump_image_erase();
            if (erase_err != ESP_OK) {
                LOGW("[崩溃诊断] 擦除 flash coredump 失败: %s", esp_err_to_name(erase_err));
            }
        }
    }
#endif

    File32 per = sd.open(panic_path, O_WRONLY | O_CREAT | O_TRUNC);
    if (per) {
        write_summary(per,
                      reset_reason,
                      abnormal,
                      PANIC_DIAG_CORE_DUMP_TO_FLASH != 0,
                      coredump_check,
                      coredump_addr,
                      coredump_size,
                      copied ? coredump_path : "",
                      copied);
        per.flush();
        per.close();
    } else {
        LOGE("[崩溃诊断] 写 panic 明细失败: %s", panic_path);
    }

    File32 sum = sd.open("/System/panic_summary.txt", O_WRONLY | O_CREAT | O_APPEND);
    if (sum) {
        write_summary(sum,
                      reset_reason,
                      abnormal,
                      PANIC_DIAG_CORE_DUMP_TO_FLASH != 0,
                      coredump_check,
                      coredump_addr,
                      coredump_size,
                      copied ? coredump_path : "",
                      copied);
        sum.flush();
        sum.close();
    } else {
        LOGE("[崩溃诊断] 写 panic_summary.txt 失败");
    }

    return copied || abnormal;
}
