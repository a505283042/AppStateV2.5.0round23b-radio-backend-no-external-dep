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

AudioOutputRoute audio_output_route_get();
const char* audio_output_route_label();

bool audio_output_route_is_headphone_only();
bool audio_output_route_is_speaker();
bool audio_output_route_is_bluetooth_tx();

/** @brief 面向用户显示/调节的音量。蓝牙发射模式下返回 BT62SP 模块音量。 */
uint8_t audio_output_route_get_user_volume();
/** @brief 设置面向用户的音量。蓝牙发射模式下只调 BT62SP，播放器输入固定安全值。 */
bool audio_output_route_set_user_volume(uint8_t value);
/** @brief 按步进调整面向用户的音量。 */
bool audio_output_route_step_user_volume(int delta);
/** @brief 蓝牙发射模式下固定送入 BT62SP 模拟输入的播放器音量。 */
uint8_t audio_output_route_bluetooth_tx_player_fixed_volume();
/** @brief 当前保存的 BT62SP 蓝牙发射音量。 */
uint8_t audio_output_route_bluetooth_tx_volume();

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