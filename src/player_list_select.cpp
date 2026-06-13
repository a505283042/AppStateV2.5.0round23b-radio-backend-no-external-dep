#include "player_list_select.h"
#include "ui/ui_list_select_view.h"
#include "ui/ui.h"

#include "keys/keys.h"
#include "menu/quick_menu.h"
#include "player_playlist.h"
#include "player_source.h"
#include "storage/storage_catalog_v3.h"
#include "storage/storage_groups_v3.h"
#include "radio/radio_catalog.h"
#include "net_music/net_music_catalog.h"
#include "utils/log.h"

// 只给本文件用的内部变量和内部函数
namespace {

PlayerListSelectHooks s_hooks{};
ListSelectState s_list_state = ListSelectState::NONE;
int s_list_selected_idx = 0;
const std::vector<PlaylistGroup>* s_list_groups = nullptr;
std::vector<PlaylistGroup> s_empty_groups;
std::vector<RadioItem> s_empty_radios;

std::vector<NetMusicItem> s_list_net_tracks;

static constexpr int NET_TRACK_PAGE_SIZE = 5;
int s_net_track_page_start_idx = 0;
int s_net_track_total = 0;

static uint32_t s_list_last_action_ms = 0;

static constexpr uint32_t LIST_TIMEOUT_GROUP_MS = 30000;  // 一级列表 30 秒
static constexpr uint32_t LIST_TIMEOUT_TRACK_MS = 60000;  // 二级列表 60 秒

// 二级歌曲列表
std::vector<TrackIndex16> s_list_tracks;

// 更新列表选择活动时间戳
static inline void list_select_touch_activity()
{
    s_list_last_action_ms = millis();
}

// 获取当前列表状态的超时时间
static inline uint32_t list_select_timeout_ms()
{
    return (s_list_state == ListSelectState::TRACKS)
             ? LIST_TIMEOUT_TRACK_MS
             : LIST_TIMEOUT_GROUP_MS;
}

// 进入列表页时强制重置列表 UI 并立即唤醒 UiTask。
// 从快捷菜单打开列表时，快捷菜单仍保持 active，屏幕上还是菜单画面；
// 如果不重置列表绘制缓存，列表绘制函数可能认为“没有变化”而直接 return，
// 造成实际已经进入列表状态，但屏幕仍停留在菜单页。
static void list_select_prepare_view_on_enter()
{
    ui_clear_list_select();
    ui_request_refresh_now();
}

// 记录二级列表的父级状态
int s_parent_group_idx = -1;
ListSelectState s_parent_group_state = ListSelectState::NONE;

// 获取当前播放列表中的组
const std::vector<PlaylistGroup>& list_select_current_groups()
{
    if (s_list_groups) return *s_list_groups;
    return s_empty_groups;
}
// 获取当前播放列表中的 radio
const std::vector<RadioItem>& list_select_current_radios()
{
    if (s_list_state == ListSelectState::RADIO) {
        return radio_catalog_items();
    }
    return s_empty_radios;
}
// 清除列表选择状态
void list_select_clear_state(bool clear_ui)
{
    s_list_state = ListSelectState::NONE;
    s_list_selected_idx = 0;
    s_list_groups = nullptr;
    s_list_tracks.clear();
    s_list_net_tracks.clear();
    s_net_track_page_start_idx = 0;
    s_net_track_total = 0;
    s_parent_group_idx = -1;
    s_parent_group_state = ListSelectState::NONE;
    s_list_last_action_ms = 0;

    if (clear_ui) {
        ui_clear_list_select();
    }
}

// 确认播放后结束列表。
// 如果列表是从快捷菜单打开的，播放确认后也同步退出快捷菜单，回到播放器界面。
void list_select_finish_confirm()
{
    const bool menu_active = quick_menu_is_active();

    list_select_clear_state(true);

    if (menu_active) {
        quick_menu_exit();
    }
}

// 长按 MODE 明确退出列表和菜单，回到播放器界面。
void list_select_exit_to_player()
{
    list_select_clear_state(true);

    if (quick_menu_is_active()) {
        quick_menu_exit();
    }
}

// 尝试播放选中的组
bool list_select_try_play_selected_group(const std::vector<PlaylistGroup>& list_groups, int group_count)
{
    if (s_list_selected_idx < 0 || s_list_selected_idx >= group_count) {
        list_select_clear_state(true);
        return false;
    }

    const int current_group_idx = s_list_selected_idx;
    const auto& group = list_groups[current_group_idx];

    LOGD("[LIST] 进入歌曲列表: %s (%d/%d)",
         playlist_group_name_cstr(storage_catalog_v3(), group),
         current_group_idx + 1, group_count);

    if (group.track_indices.empty()) {
        list_select_clear_state(true);
        return false;
    }

    s_parent_group_idx = current_group_idx;
    s_parent_group_state = s_list_state;
    s_list_tracks.assign(group.track_indices.begin(), group.track_indices.end());
    s_list_selected_idx = 0;
    s_list_state = ListSelectState::TRACKS;
    list_select_touch_activity();

    list_select_prepare_view_on_enter();

    keys_sync_to_hw_state();
    return true;
}
// 尝试播放选中的 radio
bool list_select_try_play_selected_radio(const std::vector<RadioItem>& radios, int radio_count)
{
    if (s_list_selected_idx < 0 || s_list_selected_idx >= radio_count) {
        list_select_clear_state(true);
        return false;
    }

    const int selected_radio_idx = s_list_selected_idx;
    const RadioItem& item = radios[selected_radio_idx];

    LOGI("[LIST] 确认电台: %s (%d/%d) idx=%d",
         item.name.c_str(),
         selected_radio_idx + 1,
         radio_count,
         selected_radio_idx);

    list_select_finish_confirm();

    if (s_hooks.play_radio_dispatch) {
        return s_hooks.play_radio_dispatch(selected_radio_idx);
    }
    return false;
}

bool list_select_load_net_track_page_for_selected();

void list_select_move_selection(int delta, int item_count, bool is_net_track)
{
    if (item_count <= 0) {
        return;
    }

    s_list_selected_idx = (s_list_selected_idx + delta) % item_count;
    if (s_list_selected_idx < 0) {
        s_list_selected_idx += item_count;
    }

    if (is_net_track) {
        (void)list_select_load_net_track_page_for_selected();
    }

    // 旋钮/按键移动后立刻唤醒 UiTask，避免等下一帧才看到高亮变化。
    ui_request_refresh_now();
}

bool list_select_load_net_track_page_for_selected()
{
    if (!net_music_catalog_is_loaded()) {
        (void)net_music_catalog_load();
    }

    const int total = (int)net_music_catalog_count();
    s_net_track_total = total;

    if (total <= 0) {
        s_list_net_tracks.clear();
        s_net_track_page_start_idx = 0;
        return false;
    }

    if (s_list_selected_idx < 0) {
        s_list_selected_idx = 0;
    }

    if (s_list_selected_idx >= total) {
        s_list_selected_idx = total - 1;
    }

    const int page_start = (s_list_selected_idx / NET_TRACK_PAGE_SIZE) * NET_TRACK_PAGE_SIZE;

    if (page_start == s_net_track_page_start_idx &&
        !s_list_net_tracks.empty()) {
        return true;
    }

    s_list_net_tracks.clear();
    s_list_net_tracks.reserve(NET_TRACK_PAGE_SIZE);
    s_net_track_page_start_idx = page_start;

    const int end = min(page_start + NET_TRACK_PAGE_SIZE, total);

    for (int i = page_start; i < end; ++i) {
        NetMusicItem item{};
        if (!net_music_catalog_get((uint32_t)i, &item) || !item.valid) {
            item.title = String("读取失败 #") + String(i + 1);
            item.artist = "NAS";
            item.album = "NAS";
            item.format = "mp3";
            item.valid = false;
        }
        s_list_net_tracks.push_back(item);
    }

    return !s_list_net_tracks.empty();
}

// 尝试播放选中的歌曲
bool list_select_try_play_selected_track()
{
    if (s_list_selected_idx < 0 || s_list_selected_idx >= (int)s_list_tracks.size()) {
        list_select_clear_state(true);
        return false;
    }

    const int next_track = (int)s_list_tracks[s_list_selected_idx];

    if (s_parent_group_idx >= 0) {
        player_playlist_set_current_group_idx(s_parent_group_idx);
        player_playlist_force_rebuild();
    }

    list_select_finish_confirm();

    if (s_hooks.play_track_dispatch) {
        return s_hooks.play_track_dispatch(next_track, false, true);
    }
    return false;
}

bool list_select_try_play_selected_net_track()
{
    const int total = (int)net_music_catalog_count();

    if (s_list_selected_idx < 0 || s_list_selected_idx >= total) {
        list_select_clear_state(true);
        return false;
    }

    const int selected_idx = s_list_selected_idx;

    LOGI("[LIST] 确认 NAS 歌曲: pos=%d/%d idx=%d",
        selected_idx + 1,
        total,
        selected_idx);

    list_select_finish_confirm();

    if (s_hooks.play_net_track_dispatch) {
        return s_hooks.play_net_track_dispatch(selected_idx);
    }

    return false;
}

} // 只给本文件用的内部变量和内部函数结束

// 设置列表选择模块回调
void player_list_select_setup_hooks(const PlayerListSelectHooks& hooks)
{
    s_hooks = hooks;
}
// 重置列表选择状态
void player_list_select_reset()
{
    list_select_clear_state(false);
}

bool player_list_select_enter_local_tracks()
{
    if (!storage_catalog_v3_ready()) {
        list_select_clear_state(false);
        LOGW("[LIST] 本地曲库未就绪");
        return false;
    }

    const uint32_t total_u32 = storage_catalog_v3_track_count();
    if (total_u32 == 0) {
        list_select_clear_state(false);
        LOGW("[LIST] 本地歌曲列表为空");
        return false;
    }

    if (total_u32 > UINT16_MAX) {
        list_select_clear_state(false);
        LOGW("[LIST] 本地曲库数量超过 TrackIndex16 上限: %lu", (unsigned long)total_u32);
        return false;
    }

    const int total = (int)total_u32;
    const PlayerSourceState source = player_source_get();

    s_list_groups = nullptr;
    s_list_net_tracks.clear();
    s_list_tracks.clear();
    s_list_tracks.reserve(total);
    for (int i = 0; i < total; ++i) {
        s_list_tracks.push_back((TrackIndex16)i);
    }

    s_parent_group_idx = -1;
    s_parent_group_state = ListSelectState::NONE;
    s_list_state = ListSelectState::TRACKS;
    s_list_selected_idx = source.track_idx;

    if (s_list_selected_idx < 0 || s_list_selected_idx >= total) {
        s_list_selected_idx = 0;
    }

    list_select_touch_activity();
    list_select_prepare_view_on_enter();
    keys_sync_to_hw_state();

    LOGD("[LIST] 进入本地全部歌曲列表，共 %d 首，当前 idx=%d",
         total,
         s_list_selected_idx);

    return true;
}

bool player_list_select_enter_radio()
{
    if (!radio_catalog_is_loaded()) {
        (void)radio_catalog_load();
    }

    const auto& radios = radio_catalog_items();
    if (radios.empty()) {
        list_select_clear_state(false);
        LOGW("[LIST] 电台列表为空");
        return false;
    }

    const PlayerSourceState source = player_source_get();

    s_list_groups = nullptr;
    s_list_tracks.clear();
    s_list_net_tracks.clear();
    s_list_state = ListSelectState::RADIO;
    s_list_selected_idx = source.radio_idx;

    if (s_list_selected_idx < 0 || s_list_selected_idx >= (int)radios.size()) {
        s_list_selected_idx = 0;
    }

    list_select_touch_activity();
    list_select_prepare_view_on_enter();
    keys_sync_to_hw_state();

    LOGD("[LIST] 进入电台列表，共 %d 个，当前选中 idx=%d",
         (int)radios.size(),
         s_list_selected_idx);

    return true;
}

bool player_list_select_enter_net_track()
{
    if (!net_music_catalog_is_loaded()) {
        (void)net_music_catalog_load();
    }

    const int total = (int)net_music_catalog_count();
    if (total <= 0) {
        list_select_clear_state(false);
        LOGW("[LIST] NAS 歌曲列表为空");
        return false;
    }

    const PlayerSourceState source = player_source_get();

    s_list_groups = nullptr;
    s_list_tracks.clear();
    s_list_net_tracks.clear();
    s_list_state = ListSelectState::NET_TRACK;
    s_list_selected_idx = source.net_track_idx;

    if (s_list_selected_idx < 0 || s_list_selected_idx >= total) {
        s_list_selected_idx = 0;
    }

    if (!list_select_load_net_track_page_for_selected()) {
        list_select_clear_state(false);
        return false;
    }

    list_select_touch_activity();
    list_select_prepare_view_on_enter();
    keys_sync_to_hw_state();

    LOGD("[LIST] 进入 NAS 歌曲列表，共 %d 首，当前 idx=%d",
         total,
         s_list_selected_idx);

    return true;
}

// 进入列表选择模式
bool player_list_select_enter(play_mode_t mode)
{
    const PlayerSourceState source = player_source_get();
    if (source.type == PlayerSourceType::NET_RADIO) {
        return player_list_select_enter_radio();
    }

    if (source.type == PlayerSourceType::NET_TRACK) {
        return player_list_select_enter_net_track();
    }

    if (mode == PLAY_MODE_ALL_SEQ || mode == PLAY_MODE_ALL_RND) {
        return player_list_select_enter_local_tracks();
    }

    if (mode == PLAY_MODE_ARTIST_SEQ || mode == PLAY_MODE_ARTIST_RND) {
        const auto& groups = player_playlist_artist_groups();
        if (groups.empty()) {
            list_select_clear_state(false);
            LOGW("[LIST] 歌手列表为空");
            return false;
        }

        s_list_groups = &groups;
        s_list_tracks.clear();
        s_list_net_tracks.clear();
        s_list_state = ListSelectState::ARTIST;
        s_list_selected_idx = player_playlist_get_current_group_idx();

        if (s_list_selected_idx < 0 || s_list_selected_idx >= (int)groups.size()) {
            s_list_selected_idx = 0;
        }

        list_select_touch_activity();
        list_select_prepare_view_on_enter();
        keys_sync_to_hw_state();

        LOGD("[LIST] 进入歌手列表，共 %d 个，当前选中 idx=%d",
             (int)groups.size(),
             s_list_selected_idx);

        return true;
    }

    if (mode == PLAY_MODE_ALBUM_SEQ || mode == PLAY_MODE_ALBUM_RND) {
        const auto& groups = player_playlist_album_groups();
        if (groups.empty()) {
            list_select_clear_state(false);
            LOGW("[LIST] 专辑列表为空");
            return false;
        }

        s_list_groups = &groups;
        s_list_tracks.clear();
        s_list_net_tracks.clear();
        s_list_state = ListSelectState::ALBUM;
        s_list_selected_idx = player_playlist_get_current_group_idx();

        if (s_list_selected_idx < 0 || s_list_selected_idx >= (int)groups.size()) {
            s_list_selected_idx = 0;
        }

        list_select_touch_activity();
        list_select_prepare_view_on_enter();
        keys_sync_to_hw_state();

        LOGD("[LIST] 进入专辑列表，共 %d 个，当前选中 idx=%d",
             (int)groups.size(),
             s_list_selected_idx);

        return true;
    }

    return false;
}

// 判断列表选择模块是否激活
bool player_list_select_is_active()
{
    return s_list_state != ListSelectState::NONE;
}
// 获取当前列表选择状态
ListSelectState player_list_select_get_state()
{
    return s_list_state;
}
// 获取当前选中的组索引
int player_list_select_get_selected_idx()
{
    return s_list_selected_idx;
}
// 获取当前播放列表中的组
const std::vector<PlaylistGroup>& player_list_select_get_groups()
{
    return list_select_current_groups();
}
// 获取当前播放列表中的 radio
const std::vector<RadioItem>& player_list_select_get_radios()
{
    return list_select_current_radios();
}

const std::vector<NetMusicItem>& player_list_select_get_net_tracks()
{
    return s_list_net_tracks;
}

int player_list_select_get_net_track_page_start()
{
    return s_net_track_page_start_idx;
}

int player_list_select_get_net_track_total()
{
    return s_net_track_total;
}

// 获取当前播放列表中的歌曲
const std::vector<TrackIndex16>& player_list_select_get_tracks()
{
    return s_list_tracks;
}
// 处理按键事件
void player_list_select_handle_key(key_event_t evt)
{
    if (s_list_state == ListSelectState::NONE) return;
    list_select_touch_activity();

    const bool track_level = (s_list_state == ListSelectState::TRACKS);
    const bool is_radio = (s_list_state == ListSelectState::RADIO);
    const bool is_net_track = (s_list_state == ListSelectState::NET_TRACK);

    const auto& list_groups = list_select_current_groups();
    const auto& list_radios = list_select_current_radios();

    const int item_count = is_net_track
                            ? (int)net_music_catalog_count()
                            : (track_level
                                ? (int)s_list_tracks.size()
                                : (is_radio ? (int)list_radios.size()
                                            : (int)list_groups.size()));

    if (item_count == 0) {
        list_select_clear_state(false);
        return;
    }

    switch (evt) {
        case KEY_NEXT_SHORT:
            list_select_move_selection(+1, item_count, is_net_track);
            LOGD("[LIST] 选择下一项: %d/%d", s_list_selected_idx + 1, item_count);
            break;

        case KEY_PREV_SHORT:
            list_select_move_selection(-1, item_count, is_net_track);
            LOGD("[LIST] 选择上一项: %d/%d", s_list_selected_idx + 1, item_count);
            break;

        case KEY_PAGE_DOWN_SHORT:
            list_select_move_selection(+NET_TRACK_PAGE_SIZE, item_count, is_net_track);
            LOGD("[LIST] 向下翻页: %d/%d", s_list_selected_idx + 1, item_count);
            break;

        case KEY_PAGE_UP_SHORT:
            list_select_move_selection(-NET_TRACK_PAGE_SIZE, item_count, is_net_track);
            LOGD("[LIST] 向上翻页: %d/%d", s_list_selected_idx + 1, item_count);
            break;

        case KEY_PLAY_SHORT:
            if (track_level) {
                (void)list_select_try_play_selected_track();
            } else if (is_radio) {
                (void)list_select_try_play_selected_radio(list_radios, item_count);
            } else if (is_net_track) {
                (void)list_select_try_play_selected_net_track();
            } else {
                (void)list_select_try_play_selected_group(list_groups, item_count);
            }
            break;

        case KEY_MODE_SHORT:
            if (track_level) {
                s_list_state = s_parent_group_state;
                s_list_selected_idx = (s_parent_group_idx >= 0) ? s_parent_group_idx : 0;
                s_list_tracks.clear();

                list_select_touch_activity();

                ui_clear_list_select();
                ui_request_refresh_now();

                keys_sync_to_hw_state();
                LOGD("[LIST] 返回上一级列表");
            } else {
                LOGD("[LIST] 取消选择");
                list_select_clear_state(true);
            }
            break;

        case KEY_MODE_LONG:
            LOGI("[LIST] 退出到播放器");
            list_select_exit_to_player();
            break;  
        }
}

void player_list_select_tick()
{
    if (s_list_state == ListSelectState::NONE) {
        return;
    }

    if (s_list_last_action_ms == 0) {
        list_select_touch_activity();
        return;
    }

    const uint32_t timeout_ms = list_select_timeout_ms();
    if ((uint32_t)(millis() - s_list_last_action_ms) >= timeout_ms) {
        LOGD("[LIST] 超时退出 state=%d timeout=%lu ms",
             (int)s_list_state,
             (unsigned long)timeout_ms);
        list_select_clear_state(true);
        ui_request_refresh_now();
    }
}