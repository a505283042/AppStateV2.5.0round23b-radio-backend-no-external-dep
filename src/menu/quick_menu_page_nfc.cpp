#include "menu/quick_menu_page_nfc.h"

namespace {

#define MENU_COUNT(arr) static_cast<uint8_t>(sizeof(arr) / sizeof((arr)[0]))

const QuickMenuItem NFC_ITEMS[] = {
    {"当前曲绑定NFC", QuickMenuItemType::Placeholder, QuickMenuPage::Nfc, "待接入", nullptr, nullptr, false, true},
    {"当前歌手绑定NFC", QuickMenuItemType::Placeholder, QuickMenuPage::Nfc, "待接入", nullptr, nullptr, false, true},
    {"当前专辑绑定NFC", QuickMenuItemType::Placeholder, QuickMenuPage::Nfc, "待接入", nullptr, nullptr, false, true},
    {"NFC绑定列表", QuickMenuItemType::Placeholder, QuickMenuPage::Nfc, "占位", nullptr, nullptr, false, true},
    {"清除当前绑定", QuickMenuItemType::Placeholder, QuickMenuPage::Nfc, "占位", nullptr, nullptr, false, true},
    {"返回", QuickMenuItemType::Back, QuickMenuPage::Root, "", nullptr, nullptr, true, false},
};

} // namespace

const QuickMenuPageDef& quick_menu_get_nfc_page()
{
    static const QuickMenuPageDef page = {
        "NFC",
        QuickMenuPage::Nfc,
        QuickMenuPage::Root,
        NFC_ITEMS,
        MENU_COUNT(NFC_ITEMS),
    };

    return page;
}