#pragma once

#include <Arduino.h>
#include <stdint.h>

struct PlayerPersistSnapshot {
    uint8_t version = 1;
    uint8_t volume = 100;
    uint8_t play_mode = 0;
    int current_group_idx = -1;
    int track_idx = -1;
    String track_path;
    uint8_t ui_view = 1;
    bool user_paused = true;
};

struct PlayerNetPersistSnapshot {
    uint8_t version = 1;
    uint8_t play_mode = 0;
    int track_idx = -1;
    uint32_t total_count = 0;
    uint32_t duration_ms = 0;
    String encoded_path;
    String url;
    String title;
    String artist;
    String album;
    String format;
    bool user_paused = true;
};

enum class PlayerSnapshotSource : uint8_t {
    None = 0,
    LocalTrack = 1,
    NetTrack = 2,
};

enum PlayerSnapshotRestorePollResult {
    PLAYER_SNAPSHOT_RESTORE_NONE = 0,
    PLAYER_SNAPSHOT_RESTORE_WAITING,
    PLAYER_SNAPSHOT_RESTORE_DONE,
    PLAYER_SNAPSHOT_RESTORE_FAILED,
};

// 从 NVS 同时读取本地与 NAS 两套快照；开机只自动恢复本地 UI，NAS 不在开机阶段联网。
bool player_snapshot_load_pending_from_nvs();
// 捕获当前本地或 NAS 音源状态到内存，不立即写 NVS；切换音源前调用。
bool player_snapshot_capture_current_source();
// 将本地、NAS、全局普通音量和最后音源一次性保存到 NVS。
bool player_snapshot_save_to_nvs();

/** 切换 NAS 曲库源后，只重新加载当前源自己的 NAS 快照。 */
bool player_snapshot_reload_net_context_for_active_source();

// 切换回本地前恢复本地播放模式/分组；不会启动播放。
bool player_snapshot_apply_local_context();
// 切换到 NAS 前恢复 NAS 顺序/随机模式；不会启动播放。
bool player_snapshot_apply_net_context();

// 获取已保存的本地/NAS 当前曲目。
int player_snapshot_local_track_index();
int player_snapshot_net_track_index();
// NAS 列表已加载后，按 encoded_path 校正可能变化的索引。
int player_snapshot_resolve_net_track_index(int requested_idx);

PlayerSnapshotSource player_snapshot_last_source();

// 在 player 首次进入时先恢复轻量状态，并挂起“延后恢复本地曲目”。
bool player_snapshot_begin_restore_on_player_enter();
// 轮询执行延后恢复结果；用于避开首次进入 player 的阻塞链路。
PlayerSnapshotRestorePollResult player_snapshot_poll_restore();
