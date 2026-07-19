#include "menu/quick_menu_page_nas_library.h"

#include <WiFi.h>

#include "menu/quick_menu.h"
#include "net_music/net_music_catalog.h"
#include "player_control.h"
#include "player_list_select.h"
#include "player_source.h"
#include "player_snapshot.h"
#include "web/web_server.h"

namespace {

static constexpr uint8_t kMaxMenuSources = 8;

QuickMenuItem s_items[kMaxMenuSources + 1]{};
String s_source_labels[kMaxMenuSources];
QuickMenuPageDef s_page{};

bool network_source_available()
{
    return web_wifi_is_enabled() && WiFi.status() == WL_CONNECTED;
}

const char* source_value(uint8_t idx)
{
    if (!network_source_available()) {
        return "未联网";
    }

    return idx == net_music_catalog_active_source_index()
        ? "当前"
        : "打开";
}

bool open_source(uint8_t idx)
{
    if (!network_source_available()) {
        return false;
    }

    const uint8_t active = net_music_catalog_active_source_index();
    if (idx != active) {
        // 先把旧曲库的浏览位置与播放快照写入它自己的 NVS key。
        (void)player_list_select_flush_persistent_state();
        (void)player_snapshot_save_to_nvs();

        if (player_source_type_get() == PlayerSourceType::NET_TRACK) {
            player_stop_net_track();
        }

        if (!net_music_catalog_select_source(idx)) {
            return false;
        }

        // 新曲库使用独立的播放位置和播放快照，不继承上一文件夹索引。
        (void)player_snapshot_reload_net_context_for_active_source();
    }

    // player_list_select_enter_net_track() 只会加载当前源的一份 net_music.txt。
    return player_list_select_enter_net_track();
}

const char* value_0() { return source_value(0); }
const char* value_1() { return source_value(1); }
const char* value_2() { return source_value(2); }
const char* value_3() { return source_value(3); }
const char* value_4() { return source_value(4); }
const char* value_5() { return source_value(5); }
const char* value_6() { return source_value(6); }
const char* value_7() { return source_value(7); }

bool action_0() { return open_source(0); }
bool action_1() { return open_source(1); }
bool action_2() { return open_source(2); }
bool action_3() { return open_source(3); }
bool action_4() { return open_source(4); }
bool action_5() { return open_source(5); }
bool action_6() { return open_source(6); }
bool action_7() { return open_source(7); }

const QuickMenuValueGetter kValueGetters[kMaxMenuSources] = {
    value_0, value_1, value_2, value_3,
    value_4, value_5, value_6, value_7,
};

const QuickMenuConfirmHandler kActions[kMaxMenuSources] = {
    action_0, action_1, action_2, action_3,
    action_4, action_5, action_6, action_7,
};

void rebuild_page()
{
    uint8_t count = net_music_catalog_source_count();
    if (count > kMaxMenuSources) {
        count = kMaxMenuSources;
    }

    uint8_t item_count = 0;
    for (uint8_t i = 0; i < count; ++i) {
        NetMusicSourceInfo source{};
        if (!net_music_catalog_source_get(i, &source) || !source.valid) {
            continue;
        }

        s_source_labels[item_count] = source.name;
        s_items[item_count] = {
            s_source_labels[item_count].c_str(),
            QuickMenuItemType::Action,
            QuickMenuPage::NasLibrary,
            "",
            kValueGetters[i],
            kActions[i],
            true,
            false,
        };
        ++item_count;
    }

    if (item_count == 0) {
        s_source_labels[0] = "未配置曲库源";
        s_items[item_count++] = {
            s_source_labels[0].c_str(),
            QuickMenuItemType::Status,
            QuickMenuPage::NasLibrary,
            "",
            nullptr,
            nullptr,
            false,
            false,
        };
    }

    s_items[item_count++] = {
        "返回",
        QuickMenuItemType::Back,
        QuickMenuPage::Source,
        "",
        nullptr,
        nullptr,
        true,
        false,
    };

    s_page = {
        "NAS曲库",
        QuickMenuPage::NasLibrary,
        QuickMenuPage::Source,
        s_items,
        item_count,
    };
}

} // namespace

const QuickMenuPageDef& quick_menu_get_nas_library_page()
{
    rebuild_page();
    return s_page;
}
