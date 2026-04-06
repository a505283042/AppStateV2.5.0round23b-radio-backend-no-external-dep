#include "player_list_select.h"

#include "keys/keys.h"
#include "player_playlist.h"
#include "storage/storage_catalog_v3.h"
#include "storage/storage_groups_v3.h"
#include "utils/log.h"

// 只给本文件用的内部变量和内部函数
namespace {

PlayerListSelectHooks s_hooks{};
ListSelectState s_list_state = ListSelectState::NONE;
int s_list_selected_idx = 0;
const std::vector<PlaylistGroup>* s_list_groups = nullptr;
std::vector<PlaylistGroup> s_empty_groups;

// 获取当前播放列表中的组
const std::vector<PlaylistGroup>& list_select_current_groups()
{
    if (s_list_groups) return *s_list_groups;
    return s_empty_groups;
}
// 清除列表选择状态
void list_select_clear_state(bool clear_ui)
{
    s_list_state = ListSelectState::NONE;
    s_list_selected_idx = 0;
    s_list_groups = nullptr;
    if (clear_ui) {
        ui_clear_list_select();
    }
}
// 尝试播放选中的组
bool list_select_try_play_selected_group(const std::vector<PlaylistGroup>& list_groups, int group_count)
{
    player_playlist_set_current_group_idx(s_list_selected_idx);
    const int current_group_idx = player_playlist_get_current_group_idx();
    if (current_group_idx < 0 || current_group_idx >= group_count) {
        list_select_clear_state(true);
        return false;
    }

    LOGI("[LIST] 确认选择: %s (%d/%d)",
         playlist_group_name_cstr(storage_catalog_v3(), list_groups[current_group_idx]),
         current_group_idx + 1, group_count);

    if (list_groups[current_group_idx].track_indices.empty()) {
        list_select_clear_state(true);
        return false;
    }

    player_playlist_force_rebuild();
    const auto& playlist = player_playlist_get_current();
    if (playlist.empty()) {
        list_select_clear_state(true);
        return false;
    }

    const int next_track = (int)playlist[0];
    list_select_clear_state(true);

    if (s_hooks.play_track_dispatch) {
        return s_hooks.play_track_dispatch(next_track, false, true);
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
// 进入列表选择模式
bool player_list_select_enter(play_mode_t mode)
{
    if (mode == PLAY_MODE_ARTIST_SEQ || mode == PLAY_MODE_ARTIST_RND) {
        s_list_groups = &storage_catalog_v3_artist_groups();
        if (s_list_groups->empty()) {
            list_select_clear_state(false);
            player_playlist_set_current_group_idx(0);
            return false;
        }

        s_list_selected_idx = player_playlist_get_current_group_idx();
        if (s_list_selected_idx < 0 || s_list_selected_idx >= (int)s_list_groups->size()) {
            s_list_selected_idx = 0;
        }

        s_list_state = ListSelectState::ARTIST;
        keys_sync_to_hw_state();
        LOGI("[LIST] 进入歌手列表选择模式，共 %d 个歌手，当前选中: %d",
            (int)s_list_groups->size(), s_list_selected_idx + 1);
        return true;
    }

    if (mode == PLAY_MODE_ALBUM_SEQ || mode == PLAY_MODE_ALBUM_RND) {
        s_list_groups = &storage_catalog_v3_album_groups();
        if (s_list_groups->empty()) {
            list_select_clear_state(false);
            player_playlist_set_current_group_idx(0);
            return false;
        }

        s_list_selected_idx = player_playlist_get_current_group_idx();
        if (s_list_selected_idx < 0 || s_list_selected_idx >= (int)s_list_groups->size()) {
            s_list_selected_idx = 0;
        }

        s_list_state = ListSelectState::ALBUM;
        keys_sync_to_hw_state();
        LOGI("[LIST] 进入专辑列表选择模式，共 %d 个专辑，当前选中: %d",
            (int)s_list_groups->size(), s_list_selected_idx + 1);
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
// 处理按键事件
void player_list_select_handle_key(key_event_t evt)
{
    if (s_list_state == ListSelectState::NONE) return;

    const auto& list_groups = list_select_current_groups();
    const int group_count = (int)list_groups.size();
    if (group_count == 0) {
        list_select_clear_state(false);
        return;
    }

    switch (evt) {
        case KEY_NEXT_SHORT:
            s_list_selected_idx = (s_list_selected_idx + 1) % group_count;
            LOGI("[LIST] 选择下一项: %d/%d", s_list_selected_idx + 1, group_count);
            break;

        case KEY_PREV_SHORT:
            s_list_selected_idx = (s_list_selected_idx - 1 + group_count) % group_count;
            LOGI("[LIST] 选择上一项: %d/%d", s_list_selected_idx + 1, group_count);
            break;

        case KEY_VOLUP_SHORT:
            s_list_selected_idx = (s_list_selected_idx + 5) % group_count;
            LOGI("[LIST] 向下翻页: %d/%d", s_list_selected_idx + 1, group_count);
            break;

        case KEY_VOLDN_SHORT:
            s_list_selected_idx = (s_list_selected_idx - 5 + group_count) % group_count;
            LOGI("[LIST] 向上翻页: %d/%d", s_list_selected_idx + 1, group_count);
            break;

        case KEY_PLAY_SHORT:
            (void)list_select_try_play_selected_group(list_groups, group_count);
            break;

        case KEY_MODE_SHORT:
        case KEY_MODE_LONG:
            LOGI("[LIST] 取消选择");
            list_select_clear_state(true);
            break;

        default:
            break;
    }
}
