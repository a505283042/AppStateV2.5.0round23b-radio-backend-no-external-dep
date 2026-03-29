#pragma once

#include <Arduino.h>

/**
 * @brief NFC 播放绑定分发模块。
 *
 * 负责读取一张卡对应的绑定项，并分发到：
 * - 单曲播放
 * - 歌手绑定播放
 * - 专辑绑定播放
 */
struct PlayerBindingHooks {
    bool (*play_track_dispatch)(int idx, bool verbose, bool force_cover) = nullptr;
};

/** 设置绑定播放模块回调。 */
void player_binding_setup_hooks(const PlayerBindingHooks& hooks);
/**
 * @brief 尝试消费最近一次刷到的 UID。
 * @return true 表示识别到有效绑定并已执行相应播放动作。
 */
bool player_binding_try_handle_nfc_uid(const String& uid);
/** 按歌手绑定进入歌手模式并从匹配曲目开始播放。 */
bool player_play_artist_binding(const String& artist);
/** 按专辑绑定进入专辑模式并从匹配曲目开始播放。 */
bool player_play_album_binding(const String& album);
