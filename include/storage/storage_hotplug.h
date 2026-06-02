#pragma once

#include <stdint.h>

/**
 * @brief 无 CD 脚版本的 TF 热插拔事件。
 *
 * CARD_MOUNTED：从无卡/未挂载状态探测到可挂载。
 * CARD_REMOVED：已挂载状态下确认 TF 不可访问。
 */
enum class StorageHotplugEvent : uint8_t {
    NONE = 0,
    CARD_MOUNTED,
    CARD_REMOVED,
};

void storage_hotplug_init(void);

/**
 * @brief 轮询软件热插拔状态机。
 *
 * @param allow_sd_probe 为 false 时不主动访问 SD，只保留状态。
 *        本地播放/重扫期间建议传 false，避免和高频 SD 读竞争。
 */
StorageHotplugEvent storage_hotplug_poll(bool allow_sd_probe);