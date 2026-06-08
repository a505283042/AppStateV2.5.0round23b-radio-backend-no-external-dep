#include "menu/quick_menu_page_playback.h"

#include "menu/quick_menu.h"

#include "app_flags.h"
#include "app_state.h"
#include "player_control.h"
#include "player_list_select.h"
#include "storage/storage.h"
#include "ui/ui.h"

namespace {

#define MENU_COUNT(arr) static_cast<uint8_t>(sizeof(arr) / sizeof((arr)[0]))

const char* value_play_order()
{
    return control_mode_is_random(g_play_mode) ? "随机" : "顺序";
}

const char* value_local_browse_mode()
{
    switch (g_play_mode) {
        case PLAY_MODE_ALL_SEQ:
        case PLAY_MODE_ALL_RND:
            return "全部";

        case PLAY_MODE_ARTIST_SEQ:
        case PLAY_MODE_ARTIST_RND:
            return "歌手";

        case PLAY_MODE_ALBUM_SEQ:
        case PLAY_MODE_ALBUM_RND:
            return "专辑";

        default:
            return "未知";
    }
}

const char* value_open()
{
    return "打开";
}

const char* value_execute()
{
    return "执行";
}

const char* value_tf_status()
{
    return storage_is_ready() ? "已就绪" : "未就绪";
}

bool action_toggle_play_order()
{
    ui_mode_switch_highlight();
    player_toggle_random();
    return true;
}

bool action_cycle_local_browse_mode()
{
    ui_mode_switch_highlight();
    player_cycle_mode_category();
    return true;
}

bool action_open_current_source_list()
{
    const bool ok = player_list_select_enter(g_play_mode);
    if (ok) {
        quick_menu_exit();
    }
    return ok;
}

bool action_start_rescan()
{
    const bool ok = app_request_start_rescan();
    if (ok) {
        quick_menu_exit();
    }
    return ok;
}

const QuickMenuItem PLAYBACK_ITEMS[] = {
    {"播放顺序", QuickMenuItemType::Toggle, QuickMenuPage::Playback, "", value_play_order, action_toggle_play_order, true, false},
    {"本地浏览方式", QuickMenuItemType::Toggle, QuickMenuPage::Playback, "", value_local_browse_mode, action_cycle_local_browse_mode, true, false},
    {"当前源列表", QuickMenuItemType::Action, QuickMenuPage::Playback, "", value_open, action_open_current_source_list, true, false},
    {"重扫曲库", QuickMenuItemType::Action, QuickMenuPage::Playback, "", value_execute, action_start_rescan, true, false},
    {"TF卡状态", QuickMenuItemType::Status, QuickMenuPage::Playback, "", value_tf_status, nullptr, true, false},
    {"返回", QuickMenuItemType::Back, QuickMenuPage::Root, "", nullptr, nullptr, true, false},
};

} // namespace

const QuickMenuPageDef& quick_menu_get_playback_page()
{
    static const QuickMenuPageDef page = {
        "播放控制",
        QuickMenuPage::Playback,
        QuickMenuPage::Root,
        PLAYBACK_ITEMS,
        MENU_COUNT(PLAYBACK_ITEMS),
    };

    return page;
}