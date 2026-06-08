#include "menu/quick_menu_pages.h"

#include "menu/quick_menu_page_network.h"
#include "menu/quick_menu_page_playback.h"
#include "menu/quick_menu_page_display.h"

namespace {

#define MENU_COUNT(arr) static_cast<uint8_t>(sizeof(arr) / sizeof((arr)[0]))

const QuickMenuItem ROOT_ITEMS[] = {
    {"播放控制", QuickMenuItemType::SubPage, QuickMenuPage::Playback, "", nullptr, nullptr, true, false},
    {"播放源", QuickMenuItemType::SubPage, QuickMenuPage::Source, "", nullptr, nullptr, true, false},
    {"显示设置", QuickMenuItemType::SubPage, QuickMenuPage::Display, "", nullptr, nullptr, true, false},
    {"网络设置", QuickMenuItemType::SubPage, QuickMenuPage::Network, "", nullptr, nullptr, true, false},
    {"音频输出", QuickMenuItemType::SubPage, QuickMenuPage::AudioOutput, "", nullptr, nullptr, true, false},
    {"蓝牙设置", QuickMenuItemType::SubPage, QuickMenuPage::Bluetooth, "", nullptr, nullptr, true, false},
    {"NFC", QuickMenuItemType::SubPage, QuickMenuPage::Nfc, "", nullptr, nullptr, true, false},
    {"系统信息", QuickMenuItemType::SubPage, QuickMenuPage::SystemInfo, "", nullptr, nullptr, true, false},
    {"返回", QuickMenuItemType::Back, QuickMenuPage::Root, "", nullptr, nullptr, true, false},
};


const QuickMenuItem SOURCE_ITEMS[] = {
    {"本地音乐", QuickMenuItemType::Placeholder, QuickMenuPage::Source, "待接入", nullptr, nullptr, false, true},
    {"网络电台", QuickMenuItemType::Placeholder, QuickMenuPage::Source, "待接入", nullptr, nullptr, false, true},
    {"NAS音乐", QuickMenuItemType::Placeholder, QuickMenuPage::Source, "待接入", nullptr, nullptr, false, true},
    {"返回", QuickMenuItemType::Back, QuickMenuPage::Root, "", nullptr, nullptr, true, false},
};

const QuickMenuItem AUDIO_OUTPUT_ITEMS[] = {
    {"输出路径", QuickMenuItemType::Placeholder, QuickMenuPage::AudioOutput, "占位", nullptr, nullptr, false, true},
    {"功放", QuickMenuItemType::Placeholder, QuickMenuPage::AudioOutput, "待接入", nullptr, nullptr, false, true},
    {"功放静音", QuickMenuItemType::Placeholder, QuickMenuPage::AudioOutput, "待接入", nullptr, nullptr, false, true},
    {"蓝牙发射", QuickMenuItemType::Placeholder, QuickMenuPage::AudioOutput, "待接入", nullptr, nullptr, false, true},
    {"输出测试", QuickMenuItemType::Placeholder, QuickMenuPage::AudioOutput, "占位", nullptr, nullptr, false, true},
    {"返回", QuickMenuItemType::Back, QuickMenuPage::Root, "", nullptr, nullptr, true, false},
};

const QuickMenuItem BLUETOOTH_ITEMS[] = {
    {"蓝牙电源", QuickMenuItemType::Placeholder, QuickMenuPage::Bluetooth, "待接入", nullptr, nullptr, false, true},
    {"蓝牙角色查询", QuickMenuItemType::Placeholder, QuickMenuPage::Bluetooth, "占位", nullptr, nullptr, false, true},
    {"输入模式查询", QuickMenuItemType::Placeholder, QuickMenuPage::Bluetooth, "占位", nullptr, nullptr, false, true},
    {"蓝牙名称查询", QuickMenuItemType::Placeholder, QuickMenuPage::Bluetooth, "占位", nullptr, nullptr, false, true},
    {"允许配对", QuickMenuItemType::Placeholder, QuickMenuPage::Bluetooth, "占位", nullptr, nullptr, false, true},
    {"SW配对确认", QuickMenuItemType::Placeholder, QuickMenuPage::Bluetooth, "占位", nullptr, nullptr, false, true},
    {"WKP休眠唤醒", QuickMenuItemType::Placeholder, QuickMenuPage::Bluetooth, "占位", nullptr, nullptr, false, true},
    {"蓝牙重启", QuickMenuItemType::Placeholder, QuickMenuPage::Bluetooth, "占位", nullptr, nullptr, false, true},
    {"返回", QuickMenuItemType::Back, QuickMenuPage::Root, "", nullptr, nullptr, true, false},
};

const QuickMenuItem NFC_ITEMS[] = {
    {"当前曲绑定NFC", QuickMenuItemType::Placeholder, QuickMenuPage::Nfc, "待接入", nullptr, nullptr, false, true},
    {"当前歌手绑定NFC", QuickMenuItemType::Placeholder, QuickMenuPage::Nfc, "待接入", nullptr, nullptr, false, true},
    {"当前专辑绑定NFC", QuickMenuItemType::Placeholder, QuickMenuPage::Nfc, "待接入", nullptr, nullptr, false, true},
    {"NFC绑定列表", QuickMenuItemType::Placeholder, QuickMenuPage::Nfc, "占位", nullptr, nullptr, false, true},
    {"清除当前绑定", QuickMenuItemType::Placeholder, QuickMenuPage::Nfc, "占位", nullptr, nullptr, false, true},
    {"返回", QuickMenuItemType::Back, QuickMenuPage::Root, "", nullptr, nullptr, true, false},
};

const QuickMenuItem SYSTEM_INFO_ITEMS[] = {
    {"固件版本", QuickMenuItemType::Placeholder, QuickMenuPage::SystemInfo, "待接入", nullptr, nullptr, false, true},
    {"内存状态", QuickMenuItemType::Placeholder, QuickMenuPage::SystemInfo, "待接入", nullptr, nullptr, false, true},
    {"栈状态", QuickMenuItemType::Placeholder, QuickMenuPage::SystemInfo, "待接入", nullptr, nullptr, false, true},
    {"MCP23017状态", QuickMenuItemType::Placeholder, QuickMenuPage::SystemInfo, "待接入", nullptr, nullptr, false, true},
    {"I2C/SPI状态", QuickMenuItemType::Placeholder, QuickMenuPage::SystemInfo, "占位", nullptr, nullptr, false, true},
    {"电池状态", QuickMenuItemType::Placeholder, QuickMenuPage::SystemInfo, "待接入", nullptr, nullptr, false, true},
    {"恢复默认设置", QuickMenuItemType::Placeholder, QuickMenuPage::SystemInfo, "占位", nullptr, nullptr, false, true},
    {"返回", QuickMenuItemType::Back, QuickMenuPage::Root, "", nullptr, nullptr, true, false},
};

const QuickMenuPageDef ROOT_PAGE = {
    "快捷菜单",
    QuickMenuPage::Root,
    QuickMenuPage::Root,
    ROOT_ITEMS,
    MENU_COUNT(ROOT_ITEMS),
};

const QuickMenuPageDef SOURCE_PAGE = {
    "播放源",
    QuickMenuPage::Source,
    QuickMenuPage::Root,
    SOURCE_ITEMS,
    MENU_COUNT(SOURCE_ITEMS),
};

const QuickMenuPageDef AUDIO_OUTPUT_PAGE = {
    "音频输出",
    QuickMenuPage::AudioOutput,
    QuickMenuPage::Root,
    AUDIO_OUTPUT_ITEMS,
    MENU_COUNT(AUDIO_OUTPUT_ITEMS),
};

const QuickMenuPageDef BLUETOOTH_PAGE = {
    "蓝牙设置",
    QuickMenuPage::Bluetooth,
    QuickMenuPage::Root,
    BLUETOOTH_ITEMS,
    MENU_COUNT(BLUETOOTH_ITEMS),
};

const QuickMenuPageDef NFC_PAGE = {
    "NFC",
    QuickMenuPage::Nfc,
    QuickMenuPage::Root,
    NFC_ITEMS,
    MENU_COUNT(NFC_ITEMS),
};

const QuickMenuPageDef SYSTEM_INFO_PAGE = {
    "系统信息",
    QuickMenuPage::SystemInfo,
    QuickMenuPage::Root,
    SYSTEM_INFO_ITEMS,
    MENU_COUNT(SYSTEM_INFO_ITEMS),
};

} // namespace

const QuickMenuPageDef& quick_menu_get_page_def(QuickMenuPage page)
{
    switch (page) {
        case QuickMenuPage::Playback:
            return quick_menu_get_playback_page();

        case QuickMenuPage::Source:
            return SOURCE_PAGE;

        case QuickMenuPage::Display:
            return quick_menu_get_display_page();

        case QuickMenuPage::Network:
            return quick_menu_get_network_page();

        case QuickMenuPage::AudioOutput:
            return AUDIO_OUTPUT_PAGE;

        case QuickMenuPage::Bluetooth:
            return BLUETOOTH_PAGE;

        case QuickMenuPage::Nfc:
            return NFC_PAGE;

        case QuickMenuPage::SystemInfo:
            return SYSTEM_INFO_PAGE;

        case QuickMenuPage::Root:
        default:
            return ROOT_PAGE;
    }
}