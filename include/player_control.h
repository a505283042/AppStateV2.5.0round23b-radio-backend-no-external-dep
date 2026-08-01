#pragma once

#include <stdint.h>
#include "ui/ui.h"

/**
 * @brief 播放控制动作层。
 *
 * 负责“上一首 / 下一首 / 暂停 / 音量 / 模式切换 / 自动下一首”等动作，
 * 但不负责 playlist 构建、资源补齐与 UI 渲染。
 */

struct PlayerControlHooks {
    int (*get_current_track_idx)() = nullptr;
    bool (*play_track_dispatch)(int idx, bool verbose, bool force_cover) = nullptr;
    int (*get_track_count)() = nullptr;
    bool (*enter_list_select)() = nullptr;
};

/** 设置动作层回调。 */
void player_control_setup_hooks(const PlayerControlHooks& hooks);
/** 重置“手动暂停 / 手动停止 / 自动下一首阻塞”等运行期标志。 */
void player_control_reset_runtime_flags();
/** 在新歌真正开始播放后调用，用来释放自动下一首相关保护。 */
void player_control_on_track_started();
/** 标记当前处于“用户主动暂停/恢复后应保持暂停”的状态。 */
void player_control_mark_user_paused();
/** 当前是否处于用户主动暂停态。 */
bool player_control_is_user_paused();
/** 标记本次 stop 为用户主动操作，避免被误判成“自然播完”。 */
void player_control_mark_manual_stop();
/** 当前是否应该阻塞 idle 路径里的自动推进。 */
bool player_control_should_block_idle();
/**
 * @brief 尝试执行自动下一首。
 * @param entered 是否刚进入播放器状态。
 * @param started 当前 loop 是否已经观测到成功开始播放。
 */
bool player_control_try_auto_next(bool entered, bool started);


/** 播放一个网络电台（HTTP MP3 流 MVP）。 */
bool player_play_radio_index(int idx);
/** 停止网络电台并清空电台源状态。 */
void player_stop_radio();
/** 从电台播放返回到本地播放。 */
bool player_return_from_radio_to_local();

/** NAS 播放中：切换顺序 / 随机。 */
bool player_net_track_toggle_order_random();

/** 网络播放源返回本地播放，支持 NET_RADIO / NET_TRACK。 */
bool player_return_from_network_to_local();

/** 播放一首 NAS/HTTP 网络歌曲。 */
bool player_play_net_track_index(int idx);

/** 停止 NAS/HTTP 网络歌曲并清空网络歌曲源状态。 */
void player_stop_net_track();

/** 播放当前 playlist 的下一首。 */
void player_next_track();
/** 播放当前 playlist 的上一首。 */
void player_prev_track();

/** 播放/暂停请求来源，用于定位上电误触发和跨模块控制。 */
enum class PlayerToggleTrigger : uint8_t {
    Unknown = 0,
    PlayKey,
    Hall,
    Web,
    Alarm,
    NfcAdminResume,
};

/** 当前实际播放状态，供霍尔控制和状态灯统一读取。 */
enum class PlayerPlaybackState : uint8_t {
    Stopped = 0,
    Playing,
    Paused,
};

/** 获取当前实际播放状态。 */
PlayerPlaybackState player_playback_state_get();

/**
 * @brief 幂等设置播放暂停状态。
 *
 * paused=true：已经暂停时不重复发命令；正在播放时执行暂停。
 * paused=false：已经播放时不重复发命令；暂停时恢复；停止时尝试启动当前音源。
 */
bool player_set_paused(bool paused,
                       PlayerToggleTrigger trigger = PlayerToggleTrigger::Unknown);

/**
 * @brief 切换播放/暂停。
 *
 * 用户入口语义：
 * - PlayKey 与 Web 在电磁铁开启时共用同一套摆臂驱动和霍尔到位确认；
 * - 电磁铁关闭时，PlayKey 与 Web 都直接执行普通播放/暂停切换；
 * - Alarm、NFC 管理恢复等内部来源不会驱动电磁铁。
 *
 * 普通切换语义：
 * - 正在播放 -> 暂停
 * - 已暂停 -> 继续
 * - 已停止但仍有当前歌曲 -> 重新播放当前曲目
 */
void player_toggle_play(PlayerToggleTrigger trigger = PlayerToggleTrigger::Unknown);
/** 按步长调整音量，delta 可正可负。 */
void player_volume_step(int delta);

struct PlayerSeekWindow {
    uint32_t current_ms = 0;
    uint32_t total_ms = 0;
    uint32_t playback_revision = 0;
};

/** 获取当前歌曲可跳转范围和歌曲世代。 */
bool player_seek_window_get(PlayerSeekWindow* out_window);
/**
 * 提交实体按键预览得到的绝对进度。
 * expected_playback_revision 不匹配时，AudioTask 会取消请求，避免误跳新歌曲。
 */
bool player_seek_to_ms_async(uint32_t target_ms,
                             uint32_t expected_playback_revision,
                             uint32_t* out_request_id = nullptr);
/**
 * @brief “按住编码器 + 长按 NEXT/LIST”对应的导航动作。
 *
 * 统一进入当前播放源/播放模式对应的列表：
 * - 本地全部播放：全部歌曲列表
 * - 歌手/专辑播放：对应分组列表
 * - 网络电台：电台列表
 * - NAS歌曲：NAS歌曲列表
 */
void player_next_group();
/** 只切换当前大类里的小类：顺序 <-> 随机。 */
void player_toggle_random();
/** 只切换播放大类：全部 -> 歌手 -> 专辑，并保留当前小类。 */
void player_cycle_mode_category();
/** 检查播放模式是否为随机模式。 */
bool control_mode_is_random(play_mode_t mode);
