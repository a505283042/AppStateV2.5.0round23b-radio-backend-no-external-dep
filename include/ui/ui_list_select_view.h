#pragma once

#include <vector>

#include "storage/storage_types_v3.h"
#include "radio/radio_catalog.h"

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

void ui_clear_list_select();