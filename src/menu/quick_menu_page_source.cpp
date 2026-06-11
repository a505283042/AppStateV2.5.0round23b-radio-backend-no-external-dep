#include "menu/quick_menu_page_source.h"

#include <stdio.h>

#include "menu/quick_menu.h"
#include "app_flags.h"
#include "net_music/net_music_catalog.h"
#include "player_control.h"
#include "player_list_select.h"
#include "player_source.h"
#include "radio/radio_catalog.h"
#include "ui/ui.h"

namespace {

#define MENU_COUNT(arr) static_cast<uint8_t>(sizeof(arr) / sizeof((arr)[0]))

const char* value_local_source()
{
    const PlayerSourceState source = player_source_get();
    if (source.type == PlayerSourceType::LOCAL_TRACK) {
        return "当前";
    }
    if (source.type == PlayerSourceType::NET_RADIO ||
        source.type == PlayerSourceType::NET_TRACK) {
        return "切回";
    }
    return "打开";
}

const char* value_radio_source()
{
    static char buf[16];

    // 注意：菜单 value getter 会在 UI 绘制线程里被频繁调用。
    // 这里不能为了显示数量而主动读 TF 卡，否则开机后第一次进入“播放源”页时，
    // 可能长时间占用 SD 锁，导致正在播放的本地歌曲读卡超时并误触发下一首。
    // 电台列表在 boot / 插卡流程里会预加载；如果未加载，等用户真正点击时再加载。
    if (!radio_catalog_is_loaded()) {
        return "打开";
    }

    const size_t count = radio_catalog_count();
    if (count == 0) {
        return "无列表";
    }

    snprintf(buf, sizeof(buf), "%u台", (unsigned)count);
    return buf;
}

const char* value_net_music_source()
{
    static char buf[16];

    // NAS 歌曲索引可能比较大，不能在菜单显示 value 时自动扫描。
    // 只显示已缓存的数量；未加载时显示“打开”，由点击 NAS 音乐列表时再加载。
    if (!net_music_catalog_is_loaded()) {
        return "打开";
    }

    const uint32_t count = net_music_catalog_count();
    if (count == 0) {
        return "无列表";
    }

    snprintf(buf, sizeof(buf), "%lu首", (unsigned long)count);
    return buf;
}

bool action_open_local_source()
{
    const PlayerSourceState source = player_source_get();

    if (source.type == PlayerSourceType::NET_RADIO ||
        source.type == PlayerSourceType::NET_TRACK) {
        const bool ok = player_return_from_network_to_local();
        if (ok) {
            quick_menu_exit();
        }
        return ok;
    }

    // 从菜单进入列表时不要退出快捷菜单。
    // 列表短按 MODE 清掉列表后，会自然返回当前“播放源”页面；
    // 列表长按 MODE 才会退出到播放器界面。
    return player_list_select_enter_local_tracks();
}

bool action_open_radio_source()
{
    // 保留快捷菜单会话，方便列表短按 MODE 返回“播放源”页面。
    return player_list_select_enter_radio();
}

bool action_open_net_music_source()
{
    // 保留快捷菜单会话，方便列表短按 MODE 返回“播放源”页面。
    return player_list_select_enter_net_track();
}

const QuickMenuItem SOURCE_ITEMS[] = {
    {"本地音乐", QuickMenuItemType::Action, QuickMenuPage::Source, "", value_local_source, action_open_local_source, true, false},
    {"网络电台", QuickMenuItemType::Action, QuickMenuPage::Source, "", value_radio_source, action_open_radio_source, true, false},
    {"NAS音乐", QuickMenuItemType::Action, QuickMenuPage::Source, "", value_net_music_source, action_open_net_music_source, true, false},
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