#include "menu/quick_menu_page_source.h"

namespace {

#define MENU_COUNT(arr) static_cast<uint8_t>(sizeof(arr) / sizeof((arr)[0]))

const QuickMenuItem SOURCE_ITEMS[] = {
    {"本地音乐", QuickMenuItemType::Placeholder, QuickMenuPage::Source, "待接入", nullptr, nullptr, false, true},
    {"网络电台", QuickMenuItemType::Placeholder, QuickMenuPage::Source, "待接入", nullptr, nullptr, false, true},
    {"NAS音乐", QuickMenuItemType::Placeholder, QuickMenuPage::Source, "待接入", nullptr, nullptr, false, true},
    {"返回", QuickMenuItemType::Back, QuickMenuPage::Root, "", nullptr, nullptr, true, false},
};

} // namespace

const QuickMenuPageDef& quick_menu_get_source_page()
{
    static const QuickMenuPageDef page = {
        "播放源",
        QuickMenuPage::Source,
        QuickMenuPage::Root,
        SOURCE_ITEMS,
        MENU_COUNT(SOURCE_ITEMS),
    };

    return page;
}