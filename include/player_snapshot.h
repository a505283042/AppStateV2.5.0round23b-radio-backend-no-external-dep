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

enum PlayerSnapshotRestorePollResult {
    PLAYER_SNAPSHOT_RESTORE_NONE = 0,
    PLAYER_SNAPSHOT_RESTORE_WAITING,
    PLAYER_SNAPSHOT_RESTORE_DONE,
    PLAYER_SNAPSHOT_RESTORE_FAILED,
};

// 从 NVS 读取“待恢复”的播放器快照；只做加载，不立即播放。
bool player_snapshot_load_pending_from_nvs();
// 将当前播放器关键状态保存到 NVS（固定大小 blob）。
bool player_snapshot_save_to_nvs();
// 在 player 首次进入时先恢复轻量状态，并挂起“延后恢复曲目”。
bool player_snapshot_begin_restore_on_player_enter();
// 轮询执行延后恢复结果；用于避开首次进入 player 的阻塞链路。
PlayerSnapshotRestorePollResult player_snapshot_poll_restore();
