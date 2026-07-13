#include "menu/quick_menu_page_system.h"

#include <Arduino.h>
#include <stdio.h>
#include <esp_heap_caps.h>
#include <esp32-hal-psram.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "audio/audio_service.h"
#include "hal/board_hw_control.h"
#include "hal/bq27441.h"
#include "hal/mcp23017_u3.h"
#include "hal/pcf85063.h"
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

const char* value_bq27441_status()
{
    return bq27441_is_ready() ? "OK" : "ERR";
}

const char* value_pcf85063_status()
{
    return pcf85063_is_ready() ? pcf85063_status_label() : "ERR";
}

static constexpr uint32_t RTC_MENU_CACHE_MS = 1000;
static Pcf85063Status s_rtc_cache{};
static bool s_rtc_cache_ok = false;
static uint32_t s_rtc_cache_ms = 0;

static void update_rtc_menu_cache()
{
    const uint32_t now = millis();
    if (s_rtc_cache_ms != 0 && now - s_rtc_cache_ms < RTC_MENU_CACHE_MS) {
        return;
    }
    s_rtc_cache_ok = pcf85063_read_status(&s_rtc_cache);
    s_rtc_cache_ms = now;
}

static const Pcf85063Status& rtc_menu_status()
{
    update_rtc_menu_cache();
    return s_rtc_cache;
}

const char* value_rtc_time()
{
    const Pcf85063Status& st = rtc_menu_status();
    if (!s_rtc_cache_ok || !st.time_valid) {
        if (st.oscillator_stopped) return "未设置";
        return "未知";
    }
    return pcf85063_datetime_to_text(st.time);
}

const char* value_rtc_alarm()
{
    const Pcf85063Status& st = rtc_menu_status();
    if (!s_rtc_cache_ok) return "未知";
    if (st.alarm_pending) return "已触发";
    if (st.alarm_enabled) return "已启用";
    return "关闭";
}

const char* value_rtc_int_level()
{
    const Pcf85063Status& st = rtc_menu_status();
    if (!s_rtc_cache_ok || !st.rtc_int_level_known) return "未接";
    return st.rtc_int_level ? "高" : "低";
}

const char* value_rtc_boot_alarm()
{
    return pcf85063_boot_alarm_was_pending() ? "是" : "否";
}

const char* value_rtc_control2()
{
    static char buf[16];
    const Pcf85063Status& st = rtc_menu_status();
    if (!s_rtc_cache_ok) return "未知";
    snprintf(buf, sizeof(buf), "0x%02X", static_cast<unsigned>(st.control2));
    return buf;
}

const char* value_rtc_flags_text()
{
    const Pcf85063Status& st = rtc_menu_status();
    if (!s_rtc_cache_ok) return "未知";
    if (st.oscillator_stopped) return "晶振停止";
    if (st.alarm_pending) return "闹钟触发";
    if (st.timer_pending) return "定时器触发";
    if (st.time_valid) return "正常";
    return "待校时";
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

static constexpr uint32_t BATTERY_MENU_CACHE_MS = 800;

static BatteryUiStatus s_battery_cache{};
static ChargerStatus s_charger_cache{};
static uint32_t s_battery_cache_ms = 0;

static void update_battery_menu_cache()
{
    const uint32_t now = millis();

    if (s_battery_cache_ms != 0 &&
        now - s_battery_cache_ms < BATTERY_MENU_CACHE_MS) {
        return;
    }

    board_hw_battery_status_tick();
    s_battery_cache = board_hw_get_battery_status_cached();
    s_charger_cache = board_hw_read_charger_status();
    s_battery_cache_ms = now;
}

static const BatteryUiStatus& battery_menu_sample()
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

    const BatteryUiStatus& bat = battery_menu_sample();
    if (!bat.valid || bat.mv_battery == 0) {
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

    const BatteryUiStatus& bat = battery_menu_sample();
    if (!bat.valid) {
        return "未知";
    }

    snprintf(buf, sizeof(buf), "%u%%", static_cast<unsigned>(bat.percent));
    return buf;
}

const char* value_battery_current()
{
    static char buf[20];

    const BatteryUiStatus& bat = battery_menu_sample();
    if (!bat.valid) {
        return "未知";
    }

    snprintf(buf, sizeof(buf), "%dmA", static_cast<int>(bat.average_current_ma));
    return buf;
}

const char* value_battery_capacity()
{
    static char buf[24];

    const BatteryUiStatus& bat = battery_menu_sample();
    if (!bat.valid) {
        return "未知";
    }

    if (bat.full_charge_capacity_mah > 0) {
        snprintf(buf, sizeof(buf), "%u/%umAh",
                 static_cast<unsigned>(bat.remaining_capacity_mah),
                 static_cast<unsigned>(bat.full_charge_capacity_mah));
    } else if (bat.design_capacity_mah > 0) {
        snprintf(buf, sizeof(buf), "%u/%umAh",
                 static_cast<unsigned>(bat.remaining_capacity_mah),
                 static_cast<unsigned>(bat.design_capacity_mah));
    } else {
        snprintf(buf, sizeof(buf), "%umAh",
                 static_cast<unsigned>(bat.remaining_capacity_mah));
    }
    return buf;
}

const char* value_bq_flags_text()
{
    const BatteryUiStatus& bat = battery_menu_sample();
    if (!bat.valid) {
        return "未知";
    }
    return bq27441_flags_to_text(bat.flags);
}

const char* value_bq_design_capacity()
{
    static char buf[16];
    const BatteryUiStatus& bat = battery_menu_sample();
    const uint16_t driver_capacity = bq27441_design_capacity_mah();
    const uint16_t cap = driver_capacity ? driver_capacity : bat.design_capacity_mah;
    if (cap == 0) {
        return "未知";
    }
    snprintf(buf, sizeof(buf), "%umAh", static_cast<unsigned>(cap));
    return buf;
}


static constexpr uint16_t BATTERY_CAPACITY_MIN_MAH = 100;
static constexpr uint16_t BATTERY_CAPACITY_MAX_MAH = 5000;
static constexpr uint16_t BATTERY_CAPACITY_STEP_MAH = 100;

static uint16_t s_battery_capacity_draft_mah = 500;
static bool s_battery_capacity_draft_loaded = false;
static bool s_battery_capacity_draft_dirty = false;

static void load_battery_capacity_draft_if_needed()
{
    if (s_battery_capacity_draft_loaded) return;
    s_battery_capacity_draft_mah = bq27441_target_design_capacity_mah();
    s_battery_capacity_draft_loaded = true;
    s_battery_capacity_draft_dirty = false;
}

const char* value_battery_target_capacity()
{
    static char buf[16];
    snprintf(buf,
             sizeof(buf),
             "%umAh%s",
             (unsigned)bq27441_target_design_capacity_mah(),
             s_battery_capacity_draft_dirty ? "*" : "");
    return buf;
}

const char* value_battery_capacity_draft()
{
    static char buf[16];
    load_battery_capacity_draft_if_needed();
    snprintf(buf,
             sizeof(buf),
             "%umAh%s",
             (unsigned)s_battery_capacity_draft_mah,
             s_battery_capacity_draft_dirty ? "*" : "");
    return buf;
}

const char* value_battery_capacity_edit_state()
{
    load_battery_capacity_draft_if_needed();
    return s_battery_capacity_draft_dirty ? "未保存" : "已保存";
}

bool action_battery_capacity_increase()
{
    load_battery_capacity_draft_if_needed();
    if (s_battery_capacity_draft_mah >= BATTERY_CAPACITY_MAX_MAH) return false;
    s_battery_capacity_draft_mah = static_cast<uint16_t>(
        s_battery_capacity_draft_mah + BATTERY_CAPACITY_STEP_MAH);
    s_battery_capacity_draft_dirty = true;
    return true;
}

bool action_battery_capacity_decrease()
{
    load_battery_capacity_draft_if_needed();
    if (s_battery_capacity_draft_mah <= BATTERY_CAPACITY_MIN_MAH) return false;
    s_battery_capacity_draft_mah = static_cast<uint16_t>(
        s_battery_capacity_draft_mah - BATTERY_CAPACITY_STEP_MAH);
    s_battery_capacity_draft_dirty = true;
    return true;
}

bool action_battery_capacity_default()
{
    load_battery_capacity_draft_if_needed();
    s_battery_capacity_draft_mah = 500;
    s_battery_capacity_draft_dirty =
        s_battery_capacity_draft_mah != bq27441_target_design_capacity_mah();
    return true;
}

bool action_battery_capacity_save()
{
    load_battery_capacity_draft_if_needed();
    const bool ok = bq27441_set_design_capacity_mah(s_battery_capacity_draft_mah);
    if (ok) {
        s_battery_capacity_draft_dirty = false;
        s_battery_cache_ms = 0;
    }
    return ok;
}

const char* value_battery_soh()
{
    static char buf[16];

    const BatteryUiStatus& bat = battery_menu_sample();
    if (!bat.valid || bat.state_of_health_percent == 0) {
        return "未知";
    }

    snprintf(buf, sizeof(buf), "%u%%", static_cast<unsigned>(bat.state_of_health_percent));
    return buf;
}

const char* value_bq_gpout_level()
{
    const BatteryUiStatus& bat = battery_menu_sample();
    return bat.gpout_level ? "高" : "低";
}

const char* value_bq_flags()
{
    static char buf[16];

    const BatteryUiStatus& bat = battery_menu_sample();
    if (!bat.valid) {
        return "未知";
    }

    snprintf(buf, sizeof(buf), "0x%04X", static_cast<unsigned>(bat.flags));
    return buf;
}

const char* value_battery_state()
{
    const BatteryUiStatus& bat = battery_menu_sample();

    if (!bat.valid) {
        return "未知";
    }

    if (bat.percent >= 95) {
        return "满电";
    }

    if (bat.percent >= 40) {
        return "正常";
    }

    if (bat.percent >= 20) {
        return "偏低";
    }

    if (bat.percent >= 10) {
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
    {"电池信息", QuickMenuItemType::SubPage, QuickMenuPage::BatteryInfo, "", value_battery_percent, nullptr, true, false},
    {"运行内存", QuickMenuItemType::SubPage, QuickMenuPage::MemoryInfo, "", value_open, nullptr, true, false},
    {"任务余量", QuickMenuItemType::SubPage, QuickMenuPage::StackInfo, "", value_open, nullptr, true, false},
    {"扩展芯片", QuickMenuItemType::Status, QuickMenuPage::SystemInfo, "", value_mcp23017_status, nullptr, true, false},
    {"电量计", QuickMenuItemType::Status, QuickMenuPage::SystemInfo, "", value_bq27441_status, nullptr, true, false},
    {"RTC时钟", QuickMenuItemType::SubPage, QuickMenuPage::RtcInfo, "", value_pcf85063_status, nullptr, true, false},
    {"I2C通信", QuickMenuItemType::Status, QuickMenuPage::SystemInfo, "", value_i2c_status, nullptr, true, false},
    {"恢复出厂", QuickMenuItemType::SubPage, QuickMenuPage::FactoryResetConfirm, "确认", nullptr, nullptr, true, false},
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
    {"容量设置", QuickMenuItemType::SubPage, QuickMenuPage::BatteryCapacity, "", value_battery_target_capacity, nullptr, true, false},
    {"电池电压", QuickMenuItemType::Status, QuickMenuPage::BatteryInfo, "", value_battery_voltage, nullptr, true, false},
    {"剩余电量", QuickMenuItemType::Status, QuickMenuPage::BatteryInfo, "", value_battery_percent, nullptr, true, false},
    {"电池状态", QuickMenuItemType::Status, QuickMenuPage::BatteryInfo, "", value_battery_state, nullptr, true, false},
    {"平均电流", QuickMenuItemType::Status, QuickMenuPage::BatteryInfo, "", value_battery_current, nullptr, true, false},
    {"剩余容量", QuickMenuItemType::Status, QuickMenuPage::BatteryInfo, "", value_battery_capacity, nullptr, true, false},
    {"芯片容量", QuickMenuItemType::Status, QuickMenuPage::BatteryInfo, "", value_bq_design_capacity, nullptr, true, false},
    {"健康度", QuickMenuItemType::Status, QuickMenuPage::BatteryInfo, "", value_battery_soh, nullptr, true, false},
    {"输入电源", QuickMenuItemType::Status, QuickMenuPage::BatteryInfo, "", value_external_power, nullptr, true, false},
    {"充电状态", QuickMenuItemType::Status, QuickMenuPage::BatteryInfo, "", value_charge_state, nullptr, true, false},
    {"充电检测", QuickMenuItemType::Status, QuickMenuPage::BatteryInfo, "", value_chg_level, nullptr, true, false},
    {"GPOUT电平", QuickMenuItemType::Status, QuickMenuPage::BatteryInfo, "", value_bq_gpout_level, nullptr, true, false},
    {"BQ标志", QuickMenuItemType::Status, QuickMenuPage::BatteryInfo, "", value_bq_flags, nullptr, true, false},
    {"标志说明", QuickMenuItemType::Status, QuickMenuPage::BatteryInfo, "", value_bq_flags_text, nullptr, true, false},
    {"返回", QuickMenuItemType::Back, QuickMenuPage::SystemInfo, "", nullptr, nullptr, true, false},
};

const QuickMenuItem BATTERY_CAPACITY_ITEMS[] = {
    {"目标容量", QuickMenuItemType::Status, QuickMenuPage::BatteryCapacity, "", value_battery_capacity_draft, nullptr, true, false},
    {"增加100mAh", QuickMenuItemType::Action, QuickMenuPage::BatteryCapacity, "", nullptr, action_battery_capacity_increase, true, false},
    {"减少100mAh", QuickMenuItemType::Action, QuickMenuPage::BatteryCapacity, "", nullptr, action_battery_capacity_decrease, true, false},
    {"设为500mAh", QuickMenuItemType::Action, QuickMenuPage::BatteryCapacity, "", nullptr, action_battery_capacity_default, true, false},
    {"保存并应用", QuickMenuItemType::Action, QuickMenuPage::BatteryCapacity, "", nullptr, action_battery_capacity_save, true, false},
    {"修改状态", QuickMenuItemType::Status, QuickMenuPage::BatteryCapacity, "", value_battery_capacity_edit_state, nullptr, true, false},
    {"返回", QuickMenuItemType::Back, QuickMenuPage::BatteryInfo, "", nullptr, nullptr, true, false},
};

const QuickMenuItem RTC_ITEMS[] = {
    {"RTC时间", QuickMenuItemType::Status, QuickMenuPage::RtcInfo, "", value_rtc_time, nullptr, true, false},
    {"RTC状态", QuickMenuItemType::Status, QuickMenuPage::RtcInfo, "", value_pcf85063_status, nullptr, true, false},
    {"闹钟状态", QuickMenuItemType::Status, QuickMenuPage::RtcInfo, "", value_rtc_alarm, nullptr, true, false},
    {"开机闹钟", QuickMenuItemType::Status, QuickMenuPage::RtcInfo, "", value_rtc_boot_alarm, nullptr, true, false},
    {"RTC_INT", QuickMenuItemType::Status, QuickMenuPage::RtcInfo, "", value_rtc_int_level, nullptr, true, false},
    {"控制寄存器", QuickMenuItemType::Status, QuickMenuPage::RtcInfo, "", value_rtc_control2, nullptr, true, false},
    {"状态说明", QuickMenuItemType::Status, QuickMenuPage::RtcInfo, "", value_rtc_flags_text, nullptr, true, false},
    {"返回", QuickMenuItemType::Back, QuickMenuPage::SystemInfo, "", nullptr, nullptr, true, false},
};

const QuickMenuItem FACTORY_RESET_CONFIRM_ITEMS[] = {
    {"确认清除", QuickMenuItemType::Placeholder, QuickMenuPage::FactoryResetConfirm, "待接入", nullptr, nullptr, false, true},
    {"取消返回", QuickMenuItemType::Back, QuickMenuPage::SystemInfo, "", nullptr, nullptr, true, false},
};

} // namespace

void quick_menu_reset_battery_capacity_draft()
{
    s_battery_capacity_draft_mah = bq27441_target_design_capacity_mah();
    s_battery_capacity_draft_loaded = false;
    s_battery_capacity_draft_dirty = false;
}

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
        "电池详细信息",
        QuickMenuPage::BatteryInfo,
        QuickMenuPage::SystemInfo,
        BATTERY_ITEMS,
        MENU_COUNT(BATTERY_ITEMS),
    };

    return page;
}

const QuickMenuPageDef& quick_menu_get_battery_capacity_page()
{
    static const QuickMenuPageDef page = {
        "电池容量设置",
        QuickMenuPage::BatteryCapacity,
        QuickMenuPage::BatteryInfo,
        BATTERY_CAPACITY_ITEMS,
        MENU_COUNT(BATTERY_CAPACITY_ITEMS),
    };

    return page;
}

const QuickMenuPageDef& quick_menu_get_rtc_page()
{
    static const QuickMenuPageDef page = {
        "RTC时钟",
        QuickMenuPage::RtcInfo,
        QuickMenuPage::SystemInfo,
        RTC_ITEMS,
        MENU_COUNT(RTC_ITEMS),
    };

    return page;
}

const QuickMenuPageDef& quick_menu_get_factory_reset_confirm_page()
{
    static const QuickMenuPageDef page = {
        "恢复出厂",
        QuickMenuPage::FactoryResetConfirm,
        QuickMenuPage::SystemInfo,
        FACTORY_RESET_CONFIRM_ITEMS,
        MENU_COUNT(FACTORY_RESET_CONFIRM_ITEMS),
    };

    return page;
}