#pragma once

#include <Arduino.h>
#include <vector>

#include "storage/storage_groups_v3.h"
#include "radio/radio_catalog.h"
#include "net_music/net_music_catalog.h"
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
    RADIO,
    NET_TRACK
};

/** 列表选择态内部消费的按键事件。 */
enum key_event_t {
    KEY_NEXT_SHORT,      // 下一项
    KEY_PREV_SHORT,      // 上一项
    KEY_PAGE_DOWN_SHORT, // 向下翻页
    KEY_PAGE_UP_SHORT,   // 向上翻页
    KEY_PLAY_SHORT,      // 确认选择
    KEY_MODE_SHORT,      // 返回 / 取消
    KEY_MODE_LONG        // 退出列表（长按）
};

/**
 * @brief 列表选择模块对外部播放层的唯一依赖。
 *
 * 这里不直接依赖 player_state，避免形成新的巨石耦合。
 */
struct PlayerListSelectHooks {
    bool (*play_track_dispatch)(int idx, bool verbose, bool force_cover) = nullptr;
    bool (*play_radio_dispatch)(int idx) = nullptr;
    bool (*play_net_track_dispatch)(int idx) = nullptr;
};

/** 设置回调。 */
void player_list_select_setup_hooks(const PlayerListSelectHooks& hooks);
/** 清空列表选择状态。 */
void player_list_select_reset();
/**
 * @brief 将 NAS 歌曲列表最后浏览/播放位置写入 NVS。
 *
 * 平时只更新内存，不在旋钮翻页时频繁写 flash；关机流程调用一次。
 */
bool player_list_select_flush_persistent_state();

/**
 * @brief 按当前大类模式进入列表选择。
 *
 * 全部模式进入本地全部歌曲列表；歌手/专辑模式进入对应分组列表；
 * 当前是网络源时进入对应网络列表。
 */
bool player_list_select_enter(play_mode_t mode);
/** 从快捷菜单直接进入本地全部歌曲列表，不依赖当前播放模式。 */
bool player_list_select_enter_local_tracks();
/** 从快捷菜单直接进入网络电台列表，不要求当前已经在电台播放。 */
bool player_list_select_enter_radio();
/** 从快捷菜单直接进入 NAS/HTTP 歌曲列表，不要求当前已经在 NAS 播放。 */
bool player_list_select_enter_net_track();

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

const std::vector<NetMusicItem>& player_list_select_get_net_tracks();
int player_list_select_get_net_track_page_start();
int player_list_select_get_net_track_total();

/** 在列表选择状态下处理按键事件。 */
void player_list_select_handle_key(key_event_t evt);
/** 刷新列表选择界面。 */
void player_list_select_tick();
