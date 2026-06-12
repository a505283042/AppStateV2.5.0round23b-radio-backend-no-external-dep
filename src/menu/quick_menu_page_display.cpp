#include "menu/quick_menu_page_display.h"

#include "menu/quick_menu.h"

#include "hal/board_hw_control.h"
#include "ui/ui.h"
#include "web/web_settings.h"

namespace {

#define MENU_COUNT(arr) static_cast<uint8_t>(sizeof(arr) / sizeof((arr)[0]))

const char* bool_label(bool enabled)
{
    return enabled ? "开" : "关";
}

const char* value_display_type()
{
    switch (ui_get_view()) {
        case UI_VIEW_INFO:
            return "歌词";

        case UI_VIEW_ROTATE:
            return "旋转封面";

        case UI_VIEW_COVER_PANEL:
            return "封面面板";

        default:
            return "未知";
    }
}

bool action_cycle_display_type()
{
    ui_toggle_view();
    return true;
}

const char* value_show_cover()
{
    return bool_label(web_settings_get().show_cover);
}

bool action_toggle_show_cover()
{
    WebRuntimeSettings ws = web_settings_get();
    ws.show_cover = !ws.show_cover;
    web_settings_set(ws);
    (void)web_settings_save();
    return true;
}

const char* value_cover_spin()
{
    return web_settings_get().web_cover_spin ? "开" : "关";
}

bool action_toggle_cover_spin()
{
    WebRuntimeSettings ws = web_settings_get();
    ws.web_cover_spin = !ws.web_cover_spin;
    web_settings_set(ws);
    return web_settings_save();
}

const char* value_show_next_lyric()
{
    return bool_label(web_settings_get().show_next_lyric);
}

bool action_toggle_show_next_lyric()
{
    WebRuntimeSettings ws = web_settings_get();
    ws.show_next_lyric = !ws.show_next_lyric;
    web_settings_set(ws);
    (void)web_settings_save();
    return true;
}

const char* value_backlight()
{
    return bool_label(board_hw_get_backlight());
}

bool action_toggle_backlight()
{
    const bool next_enabled = !board_hw_get_backlight();

    if (!board_hw_set_backlight(next_enabled)) {
        return false;
    }

    // 关闭背光后直接退出菜单，避免黑屏状态下继续停留在菜单里误操作。
    if (!next_enabled) {
        quick_menu_exit();
    }

    return true;
}

const QuickMenuItem DISPLAY_ITEMS[] = {
    {"显示类型", QuickMenuItemType::Toggle, QuickMenuPage::Display, "", value_display_type, action_cycle_display_type, true, false},
    {"封面旋转", QuickMenuItemType::Toggle, QuickMenuPage::Display, "", value_cover_spin, action_toggle_cover_spin, true, false},
    {"背光", QuickMenuItemType::Toggle, QuickMenuPage::Display, "", value_backlight, action_toggle_backlight, true, false},
    {"网页封面显示", QuickMenuItemType::Toggle, QuickMenuPage::Display, "", value_show_cover, action_toggle_show_cover, true, false},
    {"网页下一句歌词", QuickMenuItemType::Toggle, QuickMenuPage::Display, "", value_show_next_lyric, action_toggle_show_next_lyric, true, false},
    {"返回", QuickMenuItemType::Back, QuickMenuPage::Root, "", nullptr, nullptr, true, false},
};

} // namespace

const QuickMenuPageDef& quick_menu_get_display_page()
{
    static const QuickMenuPageDef page = {
        "显示设置",
        QuickMenuPage::Display,
        QuickMenuPage::Root,
        DISPLAY_ITEMS,
        MENU_COUNT(DISPLAY_ITEMS),
    };

    return page;
}