#pragma once

#include <stdint.h>

/**
 * @brief 播放器主状态编排层的最小公开接口。
 *
 * 经过拆分后，player_state 只保留：
 * - 播放器状态循环入口
 * - 统一播放入口
 * - 当前播放索引读取
 *
 * 其余能力已经拆到 player_control / player_playlist / player_assets /
 * player_recover / player_binding / player_list_select 等模块。
 */

/** 播放器状态机主循环入口。由 app_state 驱动。 */
void player_state_run(void);

/** 统一播放入口（基于 V3 catalog）。 */
bool player_play_idx_v3(uint32_t idx, bool verbose = false, bool force_cover = false);

/** 当前播放索引（供 NFC admin 等少量编排场景读取）。 */
int player_state_current_index(void);
void player_state_set_current_index(int idx);
