#pragma once

#include <Arduino.h>
#include <vector>

#include "storage/storage_groups_v3.h"
#include "radio/radio_catalog.h"
#include "ui/ui.h"

/**
 * @brief 分组列表选择模块。
 *
 * 用于在“歌手模式 / 专辑模式”下进入列表浏览，用户通过按键选择具体歌手/专辑，
 * 确认后再由 hook 回到播放层真正切歌。
 */

/** 列表选择状态。NONE 表示当前不在列表选择界面。 */
enum class ListSelectState {
    NONE,
    ARTIST,
    ALBUM,
    TRACKS,
    RADIO
};

/** 列表选择态内部消费的按键事件。 */
enum key_event_t {
    KEY_NEXT_SHORT,
    KEY_PREV_SHORT,
    KEY_PLAY_SHORT,
    KEY_MODE_SHORT,
    KEY_MODE_LONG,
    KEY_VOLUP_SHORT,
    KEY_VOLDN_SHORT
};

/**
 * @brief 列表选择模块对外部播放层的唯一依赖。
 *
 * 这里不直接依赖 player_state，避免形成新的巨石耦合。
 */
struct PlayerListSelectHooks {
    bool (*play_track_dispatch)(int idx, bool verbose, bool force_cover) = nullptr;
    bool (*play_radio_dispatch)(int idx) = nullptr;
};

/** 设置回调。 */
void player_list_select_setup_hooks(const PlayerListSelectHooks& hooks);
/** 清空列表选择状态。 */
void player_list_select_reset();
/**
 * @brief 按当前大类模式进入列表选择。
 * @return 仅歌手/专辑模式可进入；全部模式会返回 false。
 */
bool player_list_select_enter(play_mode_t mode);
/** 当前是否处于列表选择状态。 */
bool player_list_select_is_active();
/** 读取当前列表选择状态。 */
ListSelectState player_list_select_get_state();
/** 当前高亮项下标。 */
int player_list_select_get_selected_idx();
/** 当前正在展示的 group 列表。 */
const std::vector<PlaylistGroup>& player_list_select_get_groups();
/** 当前正在展示的 track 列表。 */
const std::vector<TrackIndex16>& player_list_select_get_tracks();
/** 当前正在展示的 radio 列表。 */
const std::vector<RadioItem>& player_list_select_get_radios();
/** 在列表选择状态下处理按键事件。 */
void player_list_select_handle_key(key_event_t evt);
/** 刷新列表选择界面。 */
void player_list_select_tick();
