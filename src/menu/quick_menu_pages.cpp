#include "menu/quick_menu_pages.h"

#include "menu/quick_menu_page_network.h"
#include "menu/quick_menu_page_playback.h"
#include "menu/quick_menu_page_display.h"
#include "menu/quick_menu_page_system.h"
#include "menu/quick_menu_page_audio_output.h"
#include "menu/quick_menu_page_source.h"
#include "menu/quick_menu_page_nfc.h"

namespace {

#define MENU_COUNT(arr) static_cast<uint8_t>(sizeof(arr) / sizeof((arr)[0]))

const QuickMenuItem ROOT_ITEMS[] = {
    {"播放控制", QuickMenuItemType::SubPage, QuickMenuPage::Playback, "", nullptr, nullptr, true, false},
    {"播放源", QuickMenuItemType::SubPage, QuickMenuPage::Source, "", nullptr, nullptr, true, false},
    {"显示设置", QuickMenuItemType::SubPage, QuickMenuPage::Display, "", nullptr, nullptr, true, false},
    {"网络设置", QuickMenuItemType::SubPage, QuickMenuPage::Network, "", nullptr, nullptr, true, false},
    {"音频输出", QuickMenuItemType::SubPage, QuickMenuPage::AudioOutput, "", nullptr, nullptr, true, false},
    {"NFC", QuickMenuItemType::SubPage, QuickMenuPage::Nfc, "", nullptr, nullptr, true, false},
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

        case QuickMenuPage::Network:
            return quick_menu_get_network_page();

        case QuickMenuPage::AudioOutput:
            return quick_menu_get_audio_output_page();

        case QuickMenuPage::Nfc:
            return quick_menu_get_nfc_page();

        case QuickMenuPage::SystemInfo:
            return quick_menu_get_system_page();

        case QuickMenuPage::MemoryInfo:
        return quick_menu_get_memory_page();

        case QuickMenuPage::StackInfo:
            return quick_menu_get_stack_page();

        case QuickMenuPage::BatteryInfo:
            return quick_menu_get_battery_page();

        case QuickMenuPage::FactoryResetConfirm:
            return quick_menu_get_factory_reset_confirm_page();

        case QuickMenuPage::Root:
        default:
            return ROOT_PAGE;
    }
}