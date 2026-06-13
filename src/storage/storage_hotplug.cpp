#include "storage/storage_hotplug.h"
#include "storage/storage.h"
#include "utils/log.h"

#include <Arduino.h>

namespace {
uint32_t s_last_probe_ms = 0;
uint8_t s_fail_count = 0;

constexpr uint32_t kMountedProbeIntervalMs = 1500;
constexpr uint32_t kSuspectProbeIntervalMs = 250;
constexpr uint32_t kUnmountedMountIntervalMs = 2500;
}

void storage_hotplug_init(void)
{
    s_last_probe_ms = millis();
    s_fail_count = 0;
}

StorageHotplugEvent storage_hotplug_poll(bool allow_sd_probe)
{
    if (!allow_sd_probe) {
        return StorageHotplugEvent::NONE;
    }

    const uint32_t now = millis();

    if (!storage_is_ready()) {
        if ((uint32_t)(now - s_last_probe_ms) < kUnmountedMountIntervalMs) {
            return StorageHotplugEvent::NONE;
        }

        s_last_probe_ms = now;

        if (storage_mount()) {
            s_fail_count = 0;
            storage_clear_io_error();
            LOGI("[TF热插拔] 卡片 已挂载");
            return StorageHotplugEvent::CARD_MOUNTED;
        }

        return StorageHotplugEvent::NONE;
    }

    const bool suspect = storage_has_recent_io_error();
    const uint32_t interval = suspect ? kSuspectProbeIntervalMs : kMountedProbeIntervalMs;

    if ((uint32_t)(now - s_last_probe_ms) < interval) {
        return StorageHotplugEvent::NONE;
    }

    s_last_probe_ms = now;

    if (storage_probe_alive()) {
        s_fail_count = 0;
        storage_clear_io_error();
        return StorageHotplugEvent::NONE;
    }

    ++s_fail_count;
    LOGW("[TF热插拔] 探测 失败 数量=%u suspect=%d", (unsigned)s_fail_count, suspect ? 1 : 0);

    if (suspect || s_fail_count >= 2) {
        s_fail_count = 0;
        LOGW("[TF热插拔] 卡片 删除d confirmed");
        return StorageHotplugEvent::CARD_REMOVED;
    }

    return StorageHotplugEvent::NONE;
}