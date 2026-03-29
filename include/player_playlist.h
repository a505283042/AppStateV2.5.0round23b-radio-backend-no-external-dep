#pragma once

#include <Arduino.h>
#include <vector>
#include <stdint.h>
#include "storage/storage_types_v3.h"
#include "ui/ui.h"

/**
 * @brief 播放列表 / 分组上下文模块。
 *
 * 负责把“全库 / 歌手 / 专辑”三类大模式与“顺序 / 随机”小模式
 * 映射成当前真正使用的 playlist，并维护当前歌曲所属 group 的上下文。
 *
 * 这个模块不负责：
 * - 切歌主入口
 * - 音频控制
 * - UI 绘制
 */

/** UI 显示用的当前位置信息（第几首 / 总数）。 */
struct PlayerPlaylistDisplayInfo {
    int display_pos = -1;
    int display_total = 0;
};

/** 初始化随机数种子，只需调用一次。 */
void player_playlist_seed_rng_once();
/** 重置 playlist 模块内部状态。通常仅在极少数全量重置场景使用。 */
void player_playlist_reset_state();
/** 强制重建当前 playlist / group 缓存。 */
void player_playlist_force_rebuild();

/** 判断给定模式是否属于“歌手大类”。 */
bool player_playlist_is_artist_mode(play_mode_t mode);
/** 判断给定模式是否属于“专辑大类”。 */
bool player_playlist_is_album_mode(play_mode_t mode);

/** 手动设置当前 group 索引。通常仅给模式切换/列表选择/恢复流程使用。 */
void player_playlist_set_current_group_idx(int group_idx);
/** 读取当前 group 索引。 */
int player_playlist_get_current_group_idx();

/** 只读访问歌手分组表。 */
const std::vector<PlaylistGroup>& player_playlist_artist_groups();
/** 只读访问专辑分组表。 */
const std::vector<PlaylistGroup>& player_playlist_album_groups();

/**
 * @brief 按给定歌曲对齐当前的歌手/专辑上下文。
 *
 * 这是模式切换、重扫恢复、刷卡播放后最关键的“围绕当前歌曲对齐语境”接口。
 */
bool player_playlist_align_group_context_for_track(int track_idx, bool verbose);

/** 根据当前歌曲刷新 playlist 状态；发现上下文不一致时会尝试自愈。 */
void player_playlist_update_for_current_track(int current_track_idx, bool verbose);
/** 读取当前真实生效的 playlist（只读）。 */
const std::vector<uint16_t>& player_playlist_get_current();
/**
 * @brief 在当前 playlist 中解析前进/后退步长。
 * @param step 正数向后，负数向前。
 * @param out_anchored 若提供，可返回这次是否因为当前歌曲不在 playlist 中而被锚定到头/尾。
 */
bool player_playlist_resolve_step(int current_track_idx,
                                  int step,
                                  int& out_track,
                                  bool* out_anchored = nullptr);
/** 给“下一首封面预读”解析目标歌曲。 */
bool player_playlist_get_next_for_cover_prefetch(int current_idx,
                                                 int& out_track_idx,
                                                 TrackInfo& out_track);

/** 聚合返回 UI 显示最常用的位置数据。 */
PlayerPlaylistDisplayInfo player_playlist_get_display_info(int current_track_idx,
                                                           int library_total_hint);
