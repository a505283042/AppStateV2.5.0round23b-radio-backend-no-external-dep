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
#include "nfc/nfc_binding.h"
#include "nfc/nfc_binding_commit.h"
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

static const char* nfc_bind_type_label_cn(NfcBindType type)
{
    switch (type) {
        case NFC_BIND_TRACK:  return "单曲";
        case NFC_BIND_ARTIST: return "歌手";
        case NFC_BIND_ALBUM:  return "专辑";
        default:              return "未知";
    }
}

static int count_bindings_for_target(NfcBindType type, const String& key)
{
    if (type == NFC_BIND_UNKNOWN || key.isEmpty()) {
        return 0;
    }

    int count = 0;
    const int total = nfc_binding_count();
    for (int i = 0; i < total; ++i) {
        NfcBindingEntry entry;
        if (!nfc_binding_get(i, entry)) {
            continue;
        }

        if (entry.type == type && entry.key == key) {
            ++count;
        }
    }
    return count;
}

static const char* value_clear_current_track()
{
    static char buf[24];

    NfcAdminTarget target;
    if (!build_current_track_target(target)) {
        return "不可用";
    }

    const int count = count_bindings_for_target(NFC_BIND_TRACK, target.key);
    if (count <= 0) {
        return "无绑定";
    }

    snprintf(buf, sizeof(buf), "%d张卡", count);
    return buf;
}

static bool action_clear_current_track_binding()
{
    NfcAdminTarget target;
    if (!build_current_track_target(target)) {
        LOGW("[MENU][NFC] 清除当前曲绑定失败: 当前没有本地歌曲");
        return false;
    }

    const int count = count_bindings_for_target(NFC_BIND_TRACK, target.key);
    if (count <= 0) {
        LOGI("[MENU][NFC] 当前曲没有 NFC 绑定: %s", target.display.c_str());
        return false;
    }

    int removed = 0;
    const bool ok = nfc_binding_remove_target_and_save_safely(NFC_BIND_TRACK,
                                                              target.key,
                                                              &removed,
                                                              nullptr,
                                                              true);
    LOGI("[MENU][NFC] 清除当前曲绑定 ok=%d removed=%d display=%s",
         ok ? 1 : 0,
         removed,
         target.display.c_str());
    return ok;
}

static constexpr int NFC_LIST_PAGE_SIZE = 5;
static int s_nfc_list_offset = 0;
static String s_nfc_detail_uid;

static int nfc_list_data_page_count()
{
    const int total = nfc_binding_count();
    if (total <= 0) {
        return 1;
    }
    return (total + NFC_LIST_PAGE_SIZE - 1) / NFC_LIST_PAGE_SIZE;
}

static int nfc_list_total_page_count()
{
    return nfc_list_data_page_count();
}

static int nfc_list_current_page_index()
{
    const int total = nfc_binding_count();
    if (total <= 0) {
        return 0;
    }

    return s_nfc_list_offset / NFC_LIST_PAGE_SIZE;
}

static void nfc_list_clamp_offset()
{
    const int total = nfc_binding_count();
    if (total <= 0) {
        s_nfc_list_offset = 0;
        return;
    }

    if (s_nfc_list_offset < 0) {
        s_nfc_list_offset = 0;
    }

    if (s_nfc_list_offset >= total) {
        s_nfc_list_offset = ((total - 1) / NFC_LIST_PAGE_SIZE) * NFC_LIST_PAGE_SIZE;
    }
}

static int nfc_list_visible_count()
{
    nfc_list_clamp_offset();

    const int total = nfc_binding_count();
    if (total <= 0) {
        return 0;
    }

    int remain = total - s_nfc_list_offset;
    if (remain > NFC_LIST_PAGE_SIZE) {
        remain = NFC_LIST_PAGE_SIZE;
    }
    if (remain < 0) {
        remain = 0;
    }
    return remain;
}

static bool nfc_list_current_page_has_next()
{
    const int total = nfc_binding_count();
    if (total <= 0) {
        return false;
    }

    return s_nfc_list_offset + NFC_LIST_PAGE_SIZE < total;
}

static String nfc_entry_name(const NfcBindingEntry& entry)
{
    String name = entry.display;
    name.trim();
    if (name.isEmpty()) {
        name = entry.key;
    }
    name.trim();
    return name;
}

static String nfc_utf8_chunk_by_bytes(const String& text, int chunk, size_t max_bytes)
{
    if (chunk < 0 || max_bytes == 0) {
        return "";
    }

    String current;
    int current_chunk = 0;

    const int len = text.length();
    for (int i = 0; i < len;) {
        const uint8_t c = static_cast<uint8_t>(text[i]);
        int cp_len = 1;
        if ((c & 0x80) == 0x00) {
            cp_len = 1;
        } else if ((c & 0xE0) == 0xC0) {
            cp_len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp_len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp_len = 4;
        }

        if (i + cp_len > len) {
            cp_len = len - i;
        }

        if (current.length() > 0 && current.length() + cp_len > max_bytes) {
            if (current_chunk == chunk) {
                return current;
            }
            ++current_chunk;
            current = "";
        }

        current += text.substring(i, i + cp_len);
        i += cp_len;
    }

    return current_chunk == chunk ? current : String("");
}

static bool nfc_get_detail_entry(NfcBindingEntry& out)
{
    if (s_nfc_detail_uid.isEmpty()) {
        return false;
    }

    return nfc_binding_find(s_nfc_detail_uid, out);
}

static void nfc_set_detail_from_slot(int slot)
{
    s_nfc_detail_uid = "";

    if (slot < 0 || slot >= NFC_LIST_PAGE_SIZE) {
        return;
    }

    nfc_list_clamp_offset();
    const int index = s_nfc_list_offset + slot;

    NfcBindingEntry entry;
    if (!nfc_binding_get(index, entry)) {
        return;
    }

    s_nfc_detail_uid = entry.uid;
}

static const char* value_nfc_list_empty()
{
    return "";
}

static const char* label_nfc_list_entry(int slot)
{
    static char bufs[NFC_LIST_PAGE_SIZE][24];

    if (slot < 0 || slot >= NFC_LIST_PAGE_SIZE) {
        return "";
    }

    nfc_list_clamp_offset();
    const int index = s_nfc_list_offset + slot;

    NfcBindingEntry entry;
    if (!nfc_binding_get(index, entry)) {
        snprintf(bufs[slot], sizeof(bufs[slot]), "#%d 空", index + 1);
        return bufs[slot];
    }

    snprintf(bufs[slot], sizeof(bufs[slot]),
             "#%d %s",
             index + 1,
             nfc_bind_type_label_cn(entry.type));
    return bufs[slot];
}

static const char* value_nfc_list_entry(int slot)
{
    static char bufs[NFC_LIST_PAGE_SIZE][96];

    if (slot < 0 || slot >= NFC_LIST_PAGE_SIZE) {
        return "";
    }

    nfc_list_clamp_offset();
    const int index = s_nfc_list_offset + slot;

    NfcBindingEntry entry;
    if (!nfc_binding_get(index, entry)) {
        return "";
    }

    String name = nfc_entry_name(entry);
    name.toCharArray(bufs[slot], sizeof(bufs[slot]));
    return bufs[slot];
}

static const char* label_nfc_list_entry_0() { return label_nfc_list_entry(0); }
static const char* label_nfc_list_entry_1() { return label_nfc_list_entry(1); }
static const char* label_nfc_list_entry_2() { return label_nfc_list_entry(2); }
static const char* label_nfc_list_entry_3() { return label_nfc_list_entry(3); }
static const char* label_nfc_list_entry_4() { return label_nfc_list_entry(4); }

static const char* value_nfc_list_entry_0() { return value_nfc_list_entry(0); }
static const char* value_nfc_list_entry_1() { return value_nfc_list_entry(1); }
static const char* value_nfc_list_entry_2() { return value_nfc_list_entry(2); }
static const char* value_nfc_list_entry_3() { return value_nfc_list_entry(3); }
static const char* value_nfc_list_entry_4() { return value_nfc_list_entry(4); }

static bool action_open_nfc_detail_slot(int slot)
{
    nfc_set_detail_from_slot(slot);
    if (s_nfc_detail_uid.isEmpty()) {
        LOGI("[MENU][NFC列表] 第%d槽为空，不能进入详情", slot + 1);
        return false;
    }

    quick_menu_open_page(QuickMenuPage::NfcDetail);
    return true;
}

static bool action_open_nfc_detail_0() { return action_open_nfc_detail_slot(0); }
static bool action_open_nfc_detail_1() { return action_open_nfc_detail_slot(1); }
static bool action_open_nfc_detail_2() { return action_open_nfc_detail_slot(2); }
static bool action_open_nfc_detail_3() { return action_open_nfc_detail_slot(3); }
static bool action_open_nfc_detail_4() { return action_open_nfc_detail_slot(4); }

static const char* value_nfc_list_prev()
{
    return s_nfc_list_offset > 0 ? "可用" : "首页";
}

static const char* value_nfc_list_next()
{
    nfc_list_clamp_offset();
    return (s_nfc_list_offset + NFC_LIST_PAGE_SIZE < nfc_binding_count()) ? "可用" : "末页";
}

static bool action_nfc_list_prev_page()
{
    const int total = nfc_binding_count();
    if (total <= 0 || s_nfc_list_offset <= 0) {
        s_nfc_list_offset = 0;
        return false;
    }

    s_nfc_list_offset -= NFC_LIST_PAGE_SIZE;
    if (s_nfc_list_offset < 0) {
        s_nfc_list_offset = 0;
    }

    // 只有真正换页时才请求整屏刷新；普通上下滚动不闪屏。
    quick_menu_request_full_refresh();
    return true;
}

static bool action_nfc_list_next_page()
{
    nfc_list_clamp_offset();
    if (!nfc_list_current_page_has_next()) {
        return false;
    }

    s_nfc_list_offset += NFC_LIST_PAGE_SIZE;
    nfc_list_clamp_offset();
    quick_menu_request_full_refresh();
    return true;
}

static const char* value_nfc_detail_uid()
{
    static char buf[40];
    NfcBindingEntry entry;
    if (!nfc_get_detail_entry(entry)) {
        return "已删除";
    }

    entry.uid.toCharArray(buf, sizeof(buf));
    return buf;
}

static const char* value_nfc_detail_type()
{
    NfcBindingEntry entry;
    if (!nfc_get_detail_entry(entry)) {
        return "无";
    }
    return nfc_bind_type_label_cn(entry.type);
}

static const char* value_nfc_detail_name()
{
    static char buf[96];
    NfcBindingEntry entry;
    if (!nfc_get_detail_entry(entry)) {
        return "";
    }

    const String name = nfc_entry_name(entry);
    name.toCharArray(buf, sizeof(buf));
    return buf;
}

static const char* value_nfc_detail_delete()
{
    NfcBindingEntry entry;
    return nfc_get_detail_entry(entry) ? "执行" : "无效";
}

static bool action_delete_nfc_detail()
{
    NfcBindingEntry entry;
    if (!nfc_get_detail_entry(entry)) {
        LOGW("[MENU][NFC详情] 删除失败: 当前详情 UID 不存在");
        return false;
    }

    LOGI("[MENU][NFC详情] 删除 UID=%s display=%s", entry.uid.c_str(), entry.display.c_str());
    const bool ok = nfc_binding_remove_and_save_safely(entry.uid, nullptr, true);
    if (ok) {
        s_nfc_detail_uid = "";
        nfc_list_clamp_offset();
        quick_menu_open_page(QuickMenuPage::NfcList);
    }
    return ok;
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
    {"NFC列表管理", QuickMenuItemType::SubPage, QuickMenuPage::NfcList, "", nullptr, nullptr, true, false},
    {"清除当前曲绑定", QuickMenuItemType::Action, QuickMenuPage::Nfc, "", value_clear_current_track, action_clear_current_track_binding, true, false},
    {"返回", QuickMenuItemType::Back, QuickMenuPage::Root, "", nullptr, nullptr, true, false},
};

    const QuickMenuItem NFC_LIST_ITEMS_EMPTY[] = {
        {"暂无绑定", QuickMenuItemType::Placeholder, QuickMenuPage::NfcList, "", value_nfc_list_empty, nullptr, true, true},
    };

    const QuickMenuItem NFC_LIST_ITEMS_1[] = {
        {"", QuickMenuItemType::Action, QuickMenuPage::NfcList, "", value_nfc_list_entry_0, action_open_nfc_detail_0, true, false, label_nfc_list_entry_0},
    };

    const QuickMenuItem NFC_LIST_ITEMS_2[] = {
        {"", QuickMenuItemType::Action, QuickMenuPage::NfcList, "", value_nfc_list_entry_0, action_open_nfc_detail_0, true, false, label_nfc_list_entry_0},
        {"", QuickMenuItemType::Action, QuickMenuPage::NfcList, "", value_nfc_list_entry_1, action_open_nfc_detail_1, true, false, label_nfc_list_entry_1},
    };

    const QuickMenuItem NFC_LIST_ITEMS_3[] = {
        {"", QuickMenuItemType::Action, QuickMenuPage::NfcList, "", value_nfc_list_entry_0, action_open_nfc_detail_0, true, false, label_nfc_list_entry_0},
        {"", QuickMenuItemType::Action, QuickMenuPage::NfcList, "", value_nfc_list_entry_1, action_open_nfc_detail_1, true, false, label_nfc_list_entry_1},
        {"", QuickMenuItemType::Action, QuickMenuPage::NfcList, "", value_nfc_list_entry_2, action_open_nfc_detail_2, true, false, label_nfc_list_entry_2},
    };

    const QuickMenuItem NFC_LIST_ITEMS_4[] = {
        {"", QuickMenuItemType::Action, QuickMenuPage::NfcList, "", value_nfc_list_entry_0, action_open_nfc_detail_0, true, false, label_nfc_list_entry_0},
        {"", QuickMenuItemType::Action, QuickMenuPage::NfcList, "", value_nfc_list_entry_1, action_open_nfc_detail_1, true, false, label_nfc_list_entry_1},
        {"", QuickMenuItemType::Action, QuickMenuPage::NfcList, "", value_nfc_list_entry_2, action_open_nfc_detail_2, true, false, label_nfc_list_entry_2},
        {"", QuickMenuItemType::Action, QuickMenuPage::NfcList, "", value_nfc_list_entry_3, action_open_nfc_detail_3, true, false, label_nfc_list_entry_3},
    };

    const QuickMenuItem NFC_LIST_ITEMS_5[] = {
        {"", QuickMenuItemType::Action, QuickMenuPage::NfcList, "", value_nfc_list_entry_0, action_open_nfc_detail_0, true, false, label_nfc_list_entry_0},
        {"", QuickMenuItemType::Action, QuickMenuPage::NfcList, "", value_nfc_list_entry_1, action_open_nfc_detail_1, true, false, label_nfc_list_entry_1},
        {"", QuickMenuItemType::Action, QuickMenuPage::NfcList, "", value_nfc_list_entry_2, action_open_nfc_detail_2, true, false, label_nfc_list_entry_2},
        {"", QuickMenuItemType::Action, QuickMenuPage::NfcList, "", value_nfc_list_entry_3, action_open_nfc_detail_3, true, false, label_nfc_list_entry_3},
        {"", QuickMenuItemType::Action, QuickMenuPage::NfcList, "", value_nfc_list_entry_4, action_open_nfc_detail_4, true, false, label_nfc_list_entry_4},
    };

        const QuickMenuItem NFC_DETAIL_ITEMS[] = {
            {"UID", QuickMenuItemType::Status, QuickMenuPage::NfcDetail, "", value_nfc_detail_uid, nullptr, true, false},
            {"类型", QuickMenuItemType::Status, QuickMenuPage::NfcDetail, "", value_nfc_detail_type, nullptr, true, false},
            {"名称", QuickMenuItemType::Status, QuickMenuPage::NfcDetail, "", value_nfc_detail_name, nullptr, true, false},
            {"删除绑定", QuickMenuItemType::Action, QuickMenuPage::NfcDetail, "", value_nfc_detail_delete, action_delete_nfc_detail, true, false},
            {"返回", QuickMenuItemType::Back, QuickMenuPage::NfcList, "", nullptr, nullptr, true, false},
        };

} // namespace

bool quick_menu_nfc_list_prev_page()
{
    return action_nfc_list_prev_page();
}

bool quick_menu_nfc_list_next_page()
{
    return action_nfc_list_next_page();
}

void quick_menu_nfc_list_reset_page()
{
    // 从 NFC管理 页面重新进入 NFC列表管理 时，从第一页开始显示。
    // 从详情页返回列表时不调用这里，保留原来的页码。
    s_nfc_list_offset = 0;
    s_nfc_detail_uid = "";
    quick_menu_request_full_refresh();
}

const QuickMenuPageDef& quick_menu_get_nfc_page()
{
    static const QuickMenuPageDef page = {
        "NFC管理",
        QuickMenuPage::Nfc,
        QuickMenuPage::Root,
        NFC_ITEMS,
        MENU_COUNT(NFC_ITEMS),
    };

    return page;
}

const QuickMenuPageDef& quick_menu_get_nfc_list_page()
{
    // NFC列表页参考普通歌曲列表：内容区固定最多 5 行，
    // 不再占用一行显示“绑定数量”，也不在列表页放“返回”。
    // 返回 NFC管理 使用 MODE 键，和歌曲列表的返回方式一致。
    static QuickMenuPageDef page = {
        "NFC列表管理",
        QuickMenuPage::NfcList,
        QuickMenuPage::Nfc,
        NFC_LIST_ITEMS_EMPTY,
        MENU_COUNT(NFC_LIST_ITEMS_EMPTY),
    };

    const int visible = nfc_list_visible_count();

    if (visible >= 5) {
        page.items = NFC_LIST_ITEMS_5;
        page.item_count = MENU_COUNT(NFC_LIST_ITEMS_5);
    } else if (visible == 4) {
        page.items = NFC_LIST_ITEMS_4;
        page.item_count = MENU_COUNT(NFC_LIST_ITEMS_4);
    } else if (visible == 3) {
        page.items = NFC_LIST_ITEMS_3;
        page.item_count = MENU_COUNT(NFC_LIST_ITEMS_3);
    } else if (visible == 2) {
        page.items = NFC_LIST_ITEMS_2;
        page.item_count = MENU_COUNT(NFC_LIST_ITEMS_2);
    } else if (visible == 1) {
        page.items = NFC_LIST_ITEMS_1;
        page.item_count = MENU_COUNT(NFC_LIST_ITEMS_1);
    } else {
        page.items = NFC_LIST_ITEMS_EMPTY;
        page.item_count = MENU_COUNT(NFC_LIST_ITEMS_EMPTY);
    }

    return page;
}

const QuickMenuPageDef& quick_menu_get_nfc_detail_page()
{
    static const QuickMenuPageDef page = {
        "NFC绑定详情",
        QuickMenuPage::NfcDetail,
        QuickMenuPage::NfcList,
        NFC_DETAIL_ITEMS,
        MENU_COUNT(NFC_DETAIL_ITEMS),
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