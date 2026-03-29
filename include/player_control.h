#pragma once

#include <stdint.h>

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

/** 播放当前 playlist 的下一首。 */
void player_next_track();
/** 播放当前 playlist 的上一首。 */
void player_prev_track();
/**
 * @brief 切换播放/暂停。
 *
 * 当前语义：
 * - 正在播放 -> 暂停
 * - 已暂停 -> 继续
 * - 已停止但仍有当前歌曲 -> 重新播放当前曲目
 */
void player_toggle_play();
/** 按步长调整音量，delta 可正可负。 */
void player_volume_step(int delta);
/**
 * @brief “NEXT 长按”对应的导航动作。
 *
 * 当前实际语义不是字面上的“切到下一个 group”：
 * - 在歌手/专辑模式里：进入列表选择界面
 * - 在全部模式里：执行一次较大的前进步长
 */
void player_next_group();
/** 只切换当前大类里的小类：顺序 <-> 随机。 */
void player_toggle_random();
/** 只切换播放大类：全部 -> 歌手 -> 专辑，并保留当前小类。 */
void player_cycle_mode_category();
