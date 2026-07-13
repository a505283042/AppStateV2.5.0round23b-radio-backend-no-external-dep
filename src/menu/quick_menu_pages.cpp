#include "menu/quick_menu_pages.h"

#include "menu/quick_menu_page_network.h"
#include "menu/quick_menu_page_playback.h"
#include "menu/quick_menu_page_display.h"
#include "menu/quick_menu_page_time_alarm.h"
#include "menu/quick_menu_page_system.h"
#include "menu/quick_menu_page_audio_output.h"
#include "menu/quick_menu_page_source.h"
#include "menu/quick_menu_page_nfc.h"

namespace {

#define MENU_COUNT(arr) static_cast<uint8_t>(sizeof(arr) / sizeof((arr)[0]))

const QuickMenuItem ROOT_ITEMS[] = {
    {"播放控制", QuickMenuItemType::SubPage, QuickMenuPage::Playback, "", nullptr, nullptr, true, false},
    {"播放来源", QuickMenuItemType::SubPage, QuickMenuPage::Source, "", nullptr, nullptr, true, false},
    {"显示设置", QuickMenuItemType::SubPage, QuickMenuPage::Display, "", nullptr, nullptr, true, false},
    {"时间与闹钟", QuickMenuItemType::SubPage, QuickMenuPage::TimeAlarm, "", nullptr, nullptr, true, false},
    {"NFC管理", QuickMenuItemType::SubPage, QuickMenuPage::Nfc, "", nullptr, nullptr, true, false},
    {"网络设置", QuickMenuItemType::SubPage, QuickMenuPage::Network, "", nullptr, nullptr, true, false},
    {"音频输出", QuickMenuItemType::SubPage, QuickMenuPage::AudioOutput, "", nullptr, nullptr, true, false},    
    {"系统信息", QuickMenuItemType::SubPage, QuickMenuPage::SystemInfo, "", nullptr, nullptr, true, false},
    {"返回", QuickMenuItemType::Back, QuickMenuPage::Root, "", nullptr, nullptr, true, false},
};

const QuickMenuPageDef ROOT_PAGE = {
    "快捷菜单",
    QuickMenuPage::Root,
    QuickMenuPage::Root,
    ROOT_ITEMS,
    MENU_COUNT(ROOT_ITEMS),
};

} // namespace

const QuickMenuPageDef& quick_menu_get_page_def(QuickMenuPage page)
{
    switch (page) {
        case QuickMenuPage::Playback:
            return quick_menu_get_playback_page();

        case QuickMenuPage::Source:
            return quick_menu_get_source_page();

        case QuickMenuPage::Display:
            return quick_menu_get_display_page();

        case QuickMenuPage::TimeAlarm:
            return quick_menu_get_time_alarm_page();

        case QuickMenuPage::AlarmSettings:
            return quick_menu_get_alarm_settings_page();

        case QuickMenuPage::AlarmTime:
            return quick_menu_get_alarm_time_page();

        case QuickMenuPage::AlarmWeekday:
            return quick_menu_get_alarm_weekday_page();

        case QuickMenuPage::Network:
            return quick_menu_get_network_page();

        case QuickMenuPage::AudioOutput:
            return quick_menu_get_audio_output_page();

        case QuickMenuPage::Nfc:
            return quick_menu_get_nfc_page();

        case QuickMenuPage::NfcList:
            return quick_menu_get_nfc_list_page();

        case QuickMenuPage::NfcDetail:
            return quick_menu_get_nfc_detail_page();

        case QuickMenuPage::SystemInfo:
            return quick_menu_get_system_page();

        case QuickMenuPage::MemoryInfo:
        return quick_menu_get_memory_page();

        case QuickMenuPage::StackInfo:
            return quick_menu_get_stack_page();

        case QuickMenuPage::BatteryInfo:
            return quick_menu_get_battery_page();

        case QuickMenuPage::BatteryCapacity:
            return quick_menu_get_battery_capacity_page();

        case QuickMenuPage::RtcInfo:
            return quick_menu_get_rtc_page();

        case QuickMenuPage::FactoryResetConfirm:
            return quick_menu_get_factory_reset_confirm_page();

        case QuickMenuPage::Root:
        default:
            return ROOT_PAGE;
    }
}