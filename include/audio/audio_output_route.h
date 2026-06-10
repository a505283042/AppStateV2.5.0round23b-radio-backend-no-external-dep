#pragma once

#include <stdint.h>

/**
 * @brief 音频输出路由。
 *
 * 3.5 耳机/Line out 是硬件常通输出，不在这里控制。
 * 这里仅管理“功放”和“蓝牙发射”二选一的可控输出。
 */
enum class AudioOutputRoute : uint8_t {
    Speaker = 0,
    BluetoothTx = 1,
};

AudioOutputRoute audio_output_route_get();
const char* audio_output_route_label();

bool audio_output_route_is_speaker();
bool audio_output_route_is_bluetooth_tx();

bool audio_output_route_select_speaker();
bool audio_output_route_select_bluetooth_tx();

/**
 * @brief 应用当前路由的硬件约束。
 *
 * 蓝牙发射模式下会强制功放静音/关断，防止音频任务重新打开功放。
 */
bool audio_output_route_enforce();

/**
 * @brief 受路由保护的功放静音控制。
 *
 * 蓝牙发射模式下不允许取消功放静音。
 */
bool audio_output_route_set_amp_mute(bool enabled);

/**
 * @brief 受路由保护的功放关断控制。
 *
 * 蓝牙发射模式下不允许释放功放关断。
 */
bool audio_output_route_set_amp_shutdown(bool enabled);