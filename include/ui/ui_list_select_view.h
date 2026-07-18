#pragma once

#include <vector>

#include "player_list_select.h"

/**
 * @brief 列表选择页绘制接口。
 *
 * 只供 UiTask / player_list_select 的列表选择链路使用。
 * 不放在 ui.h，避免普通业务模块间接依赖 PlaylistGroup / RadioItem。
 */
void ui_draw_list_select(const std::vector<PlaylistGroup>& groups,
                         int selected_idx,
                         const char* title);

void ui_draw_track_select(const std::vector<TrackIndex16>& tracks,
                          int selected_idx,
                          const char* title);

void ui_draw_radio_select(const std::vector<RadioItem>& radios,
                          int selected_idx,
                          const char* title);

void ui_draw_net_music_select(const std::vector<NetMusicItem>& items,
    int page_start_idx,
    int selected_global_idx,
    int total,
    const char* title);

/**
 * @brief 绘制已经由列表模块整理好的当前可见页。
 *
 * items 中最多包含 5 项，page_start_idx/selected_global_idx 使用全局索引，
 * 因此翻页、滚动条和选中行缓存仍与原列表行为一致。
 */
void ui_draw_list_select_snapshot(const std::vector<PlayerListSelectViewItem>& items,
                                  int page_start_idx,
                                  int selected_global_idx,
                                  int total,
                                  const char* title);

void ui_clear_list_select();
