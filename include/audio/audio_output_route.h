#pragma once

#include <stdint.h>

/**
 * @brief 音频输出路由。
 *
 * 3.5 耳机/Line out 是硬件常通输出，不在这里控制。
 * 这里管理“仅耳机 / 耳机+功放 / 耳机+蓝牙发射”三种输出路径。
 */
enum class AudioOutputRoute : uint8_t {
    HeadphoneOnly = 0,
    Speaker = 1,
    BluetoothTx = 2,
};

// 跨任务读取音频输出状态时使用纯数值快照，避免路由、音量和应用状态来自不同时间点。
struct AudioOutputRouteSnapshot {
    AudioOutputRoute route = AudioOutputRoute::Speaker;
    uint8_t bluetooth_tx_volume = 50;
    uint8_t bluetooth_tx_player_volume = 20;
    uint8_t normal_volume = 80;
    bool bluetooth_tx_volume_known = false;
    bool bluetooth_tx_policy_active = false;
    bool bluetooth_tx_apply_pending = false;
    uint32_t revision = 0;
};

AudioOutputRouteSnapshot audio_output_route_snapshot_get();

AudioOutputRoute audio_output_route_get();
const char* audio_output_route_label();

bool audio_output_route_is_headphone_only();
bool audio_output_route_is_speaker();
bool audio_output_route_is_bluetooth_tx();

/**
 * @brief 面向用户显示/调节的逻辑音量。
 *
 * 蓝牙发射模式下保持 0..100 连续语义；UART 边界会把逻辑 1..100 映射到
 * BT62SP 可听硬件区间 4..100，逻辑 0 映射为硬件静音 0。
 */
uint8_t audio_output_route_get_user_volume();
/** @brief 普通耳机/功放路线音量；蓝牙发射时返回进入蓝牙前保存的播放器音量。 */
uint8_t audio_output_route_get_normal_volume();
/** @brief 设置面向用户的逻辑音量。蓝牙发射模式下调 BT62SP，PCM 输入增益保持菜单设定值。 */
bool audio_output_route_set_user_volume(uint8_t value);
/** @brief 按步进调整面向用户的音量。 */
bool audio_output_route_step_user_volume(int delta);
/** @brief 按当前路线同步 UI；蓝牙发射音量来自 NVS，首次使用默认 50%。 */
void audio_output_route_sync_ui_volume();
/** @brief 蓝牙发射模式下送入 BT62SP 模拟输入的播放器 PCM 音量。 */
uint8_t audio_output_route_bluetooth_tx_player_volume();
/** @brief 设置蓝牙发射模拟输入的播放器 PCM 档位，范围 1..35、步进 1。 */
bool audio_output_route_set_bluetooth_tx_player_volume(uint8_t value);
/** @brief 蓝牙发射 PCM 档位按 delta 上下调节，结果始终限制在 1..35。 */
bool audio_output_route_step_bluetooth_tx_player_volume(int delta);
/** @brief 当前缓存的 BT62SP 蓝牙发射逻辑音量，而不是 UART 硬件值。 */
uint8_t audio_output_route_bluetooth_tx_volume();
/** @brief 是否已完成蓝牙发射音量初始化（NVS 保存值或首次默认值）。 */
bool audio_output_route_bluetooth_tx_volume_known();

bool audio_output_route_select_headphone_only();
bool audio_output_route_select_speaker();
bool audio_output_route_select_bluetooth_tx();

/**
 * @brief 应用当前路由的硬件约束。
 *
 * 非功放模式下会强制功放静音/关断，防止音频任务重新打开功放。
 */
bool audio_output_route_enforce();

/**
 * @brief 受路由保护的功放静音控制。
 *
 * 非功放模式下不允许取消功放静音。
 */
bool audio_output_route_set_amp_mute(bool enabled);

/**
 * @brief 受路由保护的功放关断控制。
 *
 * 非功放模式下不允许释放功放关断。
 */
bool audio_output_route_set_amp_shutdown(bool enabled);
/**
 * @brief 仅供 AudioTask 调用的底层实现。
 *
 * 这些函数会直接操作功放、蓝牙和播放器音量；其他任务必须调用上面的公开接口，
 * 由 audio_service 转发到 AudioTask，避免跨任务并发访问音频硬件。
 */
bool audio_output_route_apply_from_audio_task(AudioOutputRoute route);
bool audio_output_route_set_amp_mute_from_audio_task(bool enabled);
bool audio_output_route_set_amp_shutdown_from_audio_task(bool enabled);
/** @brief AudioTask 周期调用，接收 BT62SP 音量查询结果并同步状态。 */
void audio_output_route_poll_from_audio_task();
bool audio_output_route_set_user_volume_from_audio_task(uint8_t value);
bool audio_output_route_step_user_volume_from_audio_task(int delta);
