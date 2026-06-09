#include "menu/quick_menu_page_system.h"

#include <Arduino.h>
#include <stdio.h>
#include <esp_heap_caps.h>
#include <esp32-hal-psram.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "audio/audio_service.h"
#include "hal/board_hw_control.h"
#include "hal/mcp23017_u3.h"
#include "ui/ui.h"

namespace {

#define MENU_COUNT(arr) static_cast<uint8_t>(sizeof(arr) / sizeof((arr)[0]))

const char* value_open()
{
    return "打开";
}

const char* value_placeholder()
{
    return "占位";
}

const char* value_firmware_version()
{
    return "2.5.0";
}

const char* value_mcp23017_status()
{
    return mcp23017_u3_is_ready() ? "OK" : "ERR";
}

const char* value_i2c_status()
{
    return mcp23017_u3_is_ready() ? "OK" : "ERR";
}

const char* value_heap_free()
{
    static char buf[24];
    snprintf(buf, sizeof(buf), "%luK",
             static_cast<unsigned long>(ESP.getFreeHeap() / 1024));
    return buf;
}

const char* value_heap_min()
{
    static char buf[24];
    snprintf(buf, sizeof(buf), "%luK",
             static_cast<unsigned long>(ESP.getMinFreeHeap() / 1024));
    return buf;
}

const char* value_psram_free()
{
    static char buf[24];

    if (!psramFound()) {
        return "无";
    }

    const float mb = static_cast<float>(ESP.getFreePsram()) / 1024.0f / 1024.0f;
    snprintf(buf, sizeof(buf), "%.1fM", mb);
    return buf;
}

const char* value_psram_total()
{
    static char buf[24];

    if (!psramFound()) {
        return "无";
    }

    const float mb = static_cast<float>(ESP.getPsramSize()) / 1024.0f / 1024.0f;
    snprintf(buf, sizeof(buf), "%.1fM", mb);
    return buf;
}

const char* value_internal_free()
{
    static char buf[24];

    const uint32_t free_internal =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    snprintf(buf, sizeof(buf), "%luK",
             static_cast<unsigned long>(free_internal / 1024));

    return buf;
}

const char* value_dma_free()
{
    static char buf[24];

    const uint32_t free_dma =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);

    snprintf(buf, sizeof(buf), "%luK",
             static_cast<unsigned long>(free_dma / 1024));

    return buf;
}

const char* stack_free_label(TaskHandle_t handle)
{
    static char buf[24];

    if (!handle) {
        return "无";
    }

    const uint32_t free_bytes = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(handle));

    snprintf(buf, sizeof(buf), "%luB",
             static_cast<unsigned long>(free_bytes));

    return buf;
}

const char* value_stack_audio()
{
    return stack_free_label(audio_service_get_task_handle());
}

const char* value_stack_ui()
{
    return stack_free_label(ui_get_task_handle());
}

const char* value_stack_loop()
{
    return stack_free_label(xTaskGetHandle("loopTask"));
}

const char* value_stack_runtime()
{
    return stack_free_label(xTaskGetHandle("RuntimeMon"));
}

const char* value_stack_asset()
{
    return stack_free_label(xTaskGetHandle("PlayerAssetTask"));
}

const char* value_stack_rescan()
{
    return stack_free_label(xTaskGetHandle("rescan_v3"));
}

uint8_t battery_percent_from_mv(uint32_t mv)
{
    // 单节锂电粗略估算。不是库仑计，只用于菜单显示。
    if (mv >= 4200) return 100;
    if (mv >= 4000) return 80 + static_cast<uint8_t>((mv - 4000) * 20 / 200);
    if (mv >= 3800) return 55 + static_cast<uint8_t>((mv - 3800) * 25 / 200);
    if (mv >= 3700) return 40 + static_cast<uint8_t>((mv - 3700) * 15 / 100);
    if (mv >= 3600) return 25 + static_cast<uint8_t>((mv - 3600) * 15 / 100);
    if (mv >= 3500) return 12 + static_cast<uint8_t>((mv - 3500) * 13 / 100);
    if (mv >= 3300) return static_cast<uint8_t>((mv - 3300) * 12 / 200);
    return 0;
}

static constexpr uint32_t BATTERY_MENU_CACHE_MS = 800;

static BatterySample s_battery_cache{};
static ChargerStatus s_charger_cache{};
static uint32_t s_battery_cache_ms = 0;

static void update_battery_menu_cache()
{
    const uint32_t now = millis();

    if (s_battery_cache_ms != 0 &&
        now - s_battery_cache_ms < BATTERY_MENU_CACHE_MS) {
        return;
    }

    s_battery_cache = board_hw_read_battery();
    s_charger_cache = board_hw_read_charger_status();
    s_battery_cache_ms = now;
}

static const BatterySample& battery_menu_sample()
{
    update_battery_menu_cache();
    return s_battery_cache;
}

static const ChargerStatus& charger_menu_status()
{
    update_battery_menu_cache();
    return s_charger_cache;
}

const char* value_battery_voltage()
{
    static char buf[24];

    const BatterySample& bat = battery_menu_sample();
    if (bat.mv_battery == 0) {
        return "未知";
    }

    snprintf(buf, sizeof(buf), "%lu.%02luV",
             static_cast<unsigned long>(bat.mv_battery / 1000),
             static_cast<unsigned long>((bat.mv_battery % 1000) / 10));

    return buf;
}

const char* value_battery_percent()
{
    static char buf[16];

    const BatterySample& bat = battery_menu_sample();
    if (bat.mv_battery == 0) {
        return "未知";
    }

    snprintf(buf, sizeof(buf), "%u%%",
             static_cast<unsigned>(battery_percent_from_mv(bat.mv_battery)));

    return buf;
}

const char* value_battery_adc_mv()
{
    static char buf[24];

    const BatterySample& bat = battery_menu_sample();
    if (bat.mv_adc == 0) {
        return "未知";
    }

    snprintf(buf, sizeof(buf), "%lu.%02luV",
             static_cast<unsigned long>(bat.mv_adc / 1000),
             static_cast<unsigned long>((bat.mv_adc % 1000) / 10));

    return buf;
}

const char* value_battery_raw()
{
    static char buf[16];

    const BatterySample& bat = battery_menu_sample();
    snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(bat.raw));

    return buf;
}

const char* value_battery_state()
{
    const BatterySample& bat = battery_menu_sample();

    if (bat.mv_battery == 0) {
        return "未知";
    }

    if (bat.mv_battery >= 4100) {
        return "满电";
    }

    if (bat.mv_battery >= 3700) {
        return "正常";
    }

    if (bat.mv_battery >= 3500) {
        return "偏低";
    }

    if (bat.mv_battery >= 3300) {
        return "低电量";
    }

    return "过低";
}

const char* value_external_power()
{
    const ChargerStatus& chg = charger_menu_status();

    if (!chg.valid) {
        return "未知";
    }

    return chg.external_power_good ? "有" : "无";
}

const char* value_charge_state()
{
    const ChargerStatus& chg = charger_menu_status();

    if (!chg.valid) {
        return "未知";
    }

    if (!chg.external_power_good) {
        return "未接电";
    }

    if (chg.charging) {
        return "充电中";
    }

    // BQ25606 的 STAT 高电平通常表示充满/未充电。
    // 菜单里用更保守的说法。
    return "未充电";
}

const char* value_pg_level()
{
    const ChargerStatus& chg = charger_menu_status();

    if (!chg.valid) {
        return "未知";
    }

    return chg.pg_level ? "高" : "低";
}

const char* value_chg_level()
{
    const ChargerStatus& chg = charger_menu_status();

    if (!chg.valid) {
        return "未知";
    }

    return chg.chg_level ? "高" : "低";
}

const QuickMenuItem SYSTEM_ITEMS[] = {
    {"固件版本", QuickMenuItemType::Status, QuickMenuPage::SystemInfo, "", value_firmware_version, nullptr, true, false},
    {"运行内存", QuickMenuItemType::SubPage, QuickMenuPage::MemoryInfo, "", value_open, nullptr, true, false},
    {"任务余量", QuickMenuItemType::SubPage, QuickMenuPage::StackInfo, "", value_open, nullptr, true, false},
    {"扩展芯片", QuickMenuItemType::Status, QuickMenuPage::SystemInfo, "", value_mcp23017_status, nullptr, true, false},
    {"I2C通信", QuickMenuItemType::Status, QuickMenuPage::SystemInfo, "", value_i2c_status, nullptr, true, false},
    {"电池状态", QuickMenuItemType::SubPage, QuickMenuPage::BatteryInfo, "", value_open, nullptr, true, false},
    {"恢复出厂", QuickMenuItemType::Placeholder, QuickMenuPage::SystemInfo, "", value_placeholder, nullptr, false, true},
    {"返回", QuickMenuItemType::Back, QuickMenuPage::Root, "", nullptr, nullptr, true, false},
};

const QuickMenuItem MEMORY_ITEMS[] = {
    {"当前内存", QuickMenuItemType::Status, QuickMenuPage::MemoryInfo, "", value_heap_free, nullptr, true, false},
    {"最低内存", QuickMenuItemType::Status, QuickMenuPage::MemoryInfo, "", value_heap_min, nullptr, true, false},
    {"内部内存", QuickMenuItemType::Status, QuickMenuPage::MemoryInfo, "", value_internal_free, nullptr, true, false},
    {"音频内存", QuickMenuItemType::Status, QuickMenuPage::MemoryInfo, "", value_dma_free, nullptr, true, false},
    {"外部内存", QuickMenuItemType::Status, QuickMenuPage::MemoryInfo, "", value_psram_free, nullptr, true, false},
    {"外部总量", QuickMenuItemType::Status, QuickMenuPage::MemoryInfo, "", value_psram_total, nullptr, true, false},
    {"返回", QuickMenuItemType::Back, QuickMenuPage::SystemInfo, "", nullptr, nullptr, true, false},
};

const QuickMenuItem STACK_ITEMS[] = {
    {"音频任务", QuickMenuItemType::Status, QuickMenuPage::StackInfo, "", value_stack_audio, nullptr, true, false},
    {"屏幕任务", QuickMenuItemType::Status, QuickMenuPage::StackInfo, "", value_stack_ui, nullptr, true, false},
    {"主循环", QuickMenuItemType::Status, QuickMenuPage::StackInfo, "", value_stack_loop, nullptr, true, false},
    {"监控任务", QuickMenuItemType::Status, QuickMenuPage::StackInfo, "", value_stack_runtime, nullptr, true, false},
    {"资源任务", QuickMenuItemType::Status, QuickMenuPage::StackInfo, "", value_stack_asset, nullptr, true, false},
    {"扫描任务", QuickMenuItemType::Status, QuickMenuPage::StackInfo, "", value_stack_rescan, nullptr, true, false},
    {"返回", QuickMenuItemType::Back, QuickMenuPage::SystemInfo, "", nullptr, nullptr, true, false},
};

const QuickMenuItem BATTERY_ITEMS[] = {
    {"电池电压", QuickMenuItemType::Status, QuickMenuPage::BatteryInfo, "", value_battery_voltage, nullptr, true, false},
    {"剩余电量", QuickMenuItemType::Status, QuickMenuPage::BatteryInfo, "", value_battery_percent, nullptr, true, false},
    {"输入电源", QuickMenuItemType::Status, QuickMenuPage::BatteryInfo, "", value_external_power, nullptr, true, false},
    {"充电状态", QuickMenuItemType::Status, QuickMenuPage::BatteryInfo, "", value_charge_state, nullptr, true, false},
    {"充电检测", QuickMenuItemType::Status, QuickMenuPage::BatteryInfo, "", value_chg_level, nullptr, true, false},
    {"采样电压", QuickMenuItemType::Status, QuickMenuPage::BatteryInfo, "", value_battery_adc_mv, nullptr, true, false},
    {"采样数值", QuickMenuItemType::Status, QuickMenuPage::BatteryInfo, "", value_battery_raw, nullptr, true, false},
    {"返回", QuickMenuItemType::Back, QuickMenuPage::SystemInfo, "", nullptr, nullptr, true, false},
};

} // namespace

const QuickMenuPageDef& quick_menu_get_system_page()
{
    static const QuickMenuPageDef page = {
        "系统信息",
        QuickMenuPage::SystemInfo,
        QuickMenuPage::Root,
        SYSTEM_ITEMS,
        MENU_COUNT(SYSTEM_ITEMS),
    };

    return page;
}

const QuickMenuPageDef& quick_menu_get_memory_page()
{
    static const QuickMenuPageDef page = {
        "运行内存",
        QuickMenuPage::MemoryInfo,
        QuickMenuPage::SystemInfo,
        MEMORY_ITEMS,
        MENU_COUNT(MEMORY_ITEMS),
    };

    return page;
}

const QuickMenuPageDef& quick_menu_get_stack_page()
{
    static const QuickMenuPageDef page = {
        "任务余量",
        QuickMenuPage::StackInfo,
        QuickMenuPage::SystemInfo,
        STACK_ITEMS,
        MENU_COUNT(STACK_ITEMS),
    };

    return page;
}

const QuickMenuPageDef& quick_menu_get_battery_page()
{
    static const QuickMenuPageDef page = {
        "电池状态",
        QuickMenuPage::BatteryInfo,
        QuickMenuPage::SystemInfo,
        BATTERY_ITEMS,
        MENU_COUNT(BATTERY_ITEMS),
    };

    return page;
}