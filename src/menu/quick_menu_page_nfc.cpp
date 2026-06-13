#include "menu/quick_menu_page_nfc.h"

#include "app_flags.h"
#include "app_state.h"
#include "menu/quick_menu.h"
#include "player_playlist.h"
#include "player_source.h"
#include "player_state.h"
#include "storage/storage_catalog_v3.h"
#include "storage/storage_groups_v3.h"
#include "ui/ui.h"
#include "utils/log.h"

#include <vector>

namespace {

#define MENU_COUNT(arr) static_cast<uint8_t>(sizeof(arr) / sizeof((arr)[0]))

const char* value_bind_action()
{
    return "刷卡";
}

static bool get_current_local_track_index(int& out_track_idx)
{
    out_track_idx = -1;

    if (!storage_catalog_v3_ready()) {
        LOGW("[MENU][NFC] no catalog ready");
        return false;
    }

    const PlayerSourceState source = player_source_get();
    if (source.type != PlayerSourceType::LOCAL_TRACK) {
        LOGW("[MENU][NFC] bind denied: current source is %s", player_source_type_key(source.type));
        return false;
    }

    int track_idx = player_state_current_index();
    if (track_idx < 0 && source.track_idx >= 0) {
        track_idx = source.track_idx;
    }

    const int track_count = static_cast<int>(storage_catalog_v3_track_count());
    if (track_idx < 0 || track_idx >= track_count) {
        LOGW("[MENU][NFC] bind denied: invalid current track idx=%d count=%d", track_idx, track_count);
        return false;
    }

    out_track_idx = track_idx;
    return true;
}

static bool group_contains_track(const PlaylistGroup& group, int track_idx)
{
    for (const TrackIndex16 item : group.track_indices) {
        if (static_cast<int>(item) == track_idx) {
            return true;
        }
    }
    return false;
}

static int find_group_for_track(const std::vector<PlaylistGroup>& groups,
                                int track_idx,
                                int preferred_idx)
{
    // 如果当前播放上下文正好在同类分组里，优先使用当前分组。
    if (preferred_idx >= 0 && preferred_idx < static_cast<int>(groups.size())) {
        if (group_contains_track(groups[preferred_idx], track_idx)) {
            return preferred_idx;
        }
    }

    // 否则从分组表里找第一组包含当前歌曲的记录。
    for (int i = 0; i < static_cast<int>(groups.size()); ++i) {
        if (group_contains_track(groups[i], track_idx)) {
            return i;
        }
    }

    return -1;
}

static bool build_current_track_target(NfcAdminTarget& target)
{
    target = NfcAdminTarget{};

    int track_idx = -1;
    if (!get_current_local_track_index(track_idx)) {
        return false;
    }

    TrackViewV3 view;
    if (!storage_catalog_v3_get_track_view(static_cast<uint32_t>(track_idx), view)) {
        LOGW("[MENU][NFC] get track view failed idx=%d", track_idx);
        return false;
    }

    target.type = NFC_ADMIN_TARGET_TRACK;
    target.track_idx = track_idx;
    target.key = view.audio_path;
    target.display = view.title;
    if (view.artist.length() > 0) {
        target.display += " - ";
        target.display += view.artist;
    }

    if (target.key.isEmpty()) {
        LOGW("[MENU][NFC] track target empty path idx=%d", track_idx);
        return false;
    }

    return true;
}

static bool build_current_artist_target(NfcAdminTarget& target)
{
    target = NfcAdminTarget{};

    int track_idx = -1;
    if (!get_current_local_track_index(track_idx)) {
        return false;
    }

    const MusicCatalogV3& cat = storage_catalog_v3();
    const auto& groups = player_playlist_artist_groups();
    const int preferred_idx = player_playlist_is_artist_mode(g_play_mode)
        ? player_playlist_get_current_group_idx()
        : -1;
    const int group_idx = find_group_for_track(groups, track_idx, preferred_idx);

    if (group_idx < 0) {
        LOGW("[MENU][NFC] artist group not found for track=%d", track_idx);
        return false;
    }

    target.type = NFC_ADMIN_TARGET_ARTIST;
    target.track_idx = track_idx;
    target.key = playlist_group_name_string(cat, groups[group_idx]);
    target.display = target.key;

    if (target.key.isEmpty()) {
        LOGW("[MENU][NFC] artist target empty group=%d track=%d", group_idx, track_idx);
        return false;
    }

    return true;
}

static bool build_current_album_target(NfcAdminTarget& target)
{
    target = NfcAdminTarget{};

    int track_idx = -1;
    if (!get_current_local_track_index(track_idx)) {
        return false;
    }

    const MusicCatalogV3& cat = storage_catalog_v3();
    const auto& groups = player_playlist_album_groups();
    const int preferred_idx = player_playlist_is_album_mode(g_play_mode)
        ? player_playlist_get_current_group_idx()
        : -1;
    const int group_idx = find_group_for_track(groups, track_idx, preferred_idx);

    if (group_idx < 0) {
        LOGW("[MENU][NFC] album group not found for track=%d", track_idx);
        return false;
    }

    target.type = NFC_ADMIN_TARGET_ALBUM;
    target.track_idx = track_idx;
    target.key = playlist_group_display_string(cat, groups[group_idx]);
    target.display = target.key;

    if (target.key.isEmpty()) {
        LOGW("[MENU][NFC] album target empty group=%d track=%d", group_idx, track_idx);
        return false;
    }

    return true;
}

static bool enter_nfc_admin_from_menu(const NfcAdminTarget& target)
{
    // 进入 NFC 管理状态前必须退出快捷菜单。
    // 否则 keys_update() 会优先处理 quick_menu，导致 PLAY / MODE 无法转给 NFC admin。
    quick_menu_exit();
    return app_request_enter_nfc_admin_with_target(target);
}

static bool action_bind_current_track()
{
    NfcAdminTarget target;
    if (!build_current_track_target(target)) {
        return false;
    }

    LOGI("[MENU][NFC] bind current track: %s", target.display.c_str());
    return enter_nfc_admin_from_menu(target);
}

static bool action_bind_current_artist()
{
    NfcAdminTarget target;
    if (!build_current_artist_target(target)) {
        return false;
    }

    LOGI("[MENU][NFC] bind current artist: %s", target.display.c_str());
    return enter_nfc_admin_from_menu(target);
}

static bool action_bind_current_album()
{
    NfcAdminTarget target;
    if (!build_current_album_target(target)) {
        return false;
    }

    LOGI("[MENU][NFC] bind current album: %s", target.display.c_str());
    return enter_nfc_admin_from_menu(target);
}

const QuickMenuItem NFC_ITEMS[] = {
    {"当前曲绑定NFC", QuickMenuItemType::Action, QuickMenuPage::Nfc, "", value_bind_action, action_bind_current_track, true, false},
    {"当前歌手绑定NFC", QuickMenuItemType::Action, QuickMenuPage::Nfc, "", value_bind_action, action_bind_current_artist, true, false},
    {"当前专辑绑定NFC", QuickMenuItemType::Action, QuickMenuPage::Nfc, "", value_bind_action, action_bind_current_album, true, false},
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

bool quick_menu_nfc_bind_current_track()
{
    return action_bind_current_track();
}

bool quick_menu_nfc_bind_current_artist()
{
    return action_bind_current_artist();
}

bool quick_menu_nfc_bind_current_album()
{
    return action_bind_current_album();
}