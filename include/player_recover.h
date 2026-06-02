#pragma once

#include <Arduino.h>

/**
 * @brief 重扫恢复 / 路径索引模块。
 *
 * 负责：
 * - 维护 path -> track_idx 的快速索引
 * - 在重扫前保存当前歌曲路径
 * - 在重扫结束后恢复到原曲并重建 playlist 上下文
 */
struct PlayerRecoverHooks {
    int (*get_current_track_idx)() = nullptr;
    bool (*play_track_dispatch)(int idx, bool verbose, bool force_cover) = nullptr;
};

/** 设置恢复模块回调。 */
void player_recover_setup_hooks(const PlayerRecoverHooks& hooks);
/** generation 变化后重建路径索引；未变化时会直接复用。 */
void player_recover_rebuild_path_index_if_needed();
/** 根据完整音频路径查 track_idx。 */
int player_recover_find_track_idx_by_path(const String& full_path);
/** 读取当前曲目的完整路径，用于重扫前保存恢复点。 */
bool player_recover_get_current_track_path(String& out_path);
/** 在启动重扫前记录当前歌曲路径。 */
void player_recover_prepare_rescan_restore_current();
/**
 * @brief 在 loop 中检查“重扫已完成”并执行恢复。
 * @return 返回 true 代表本轮消费了 rescan_done 事件。
 */
bool player_recover_try_handle_rescan_done();
