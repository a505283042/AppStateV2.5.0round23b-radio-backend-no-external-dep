#pragma once

#include <Arduino.h>
#include "nfc/nfc_binding.h"

/**
 * @brief 统一的 NFC 绑定安全提交入口。
 *
 * 在保存 /System/config/nfc_map.txt 前，先按项目现有安全策略停音频，避免与本地播放同时占用 SD。
 * was_playing_before 非空时，会返回提交前是否处于“正在播放且未暂停”的状态，
 * 供上层决定是否在提交完成后恢复播放。
 */
bool nfc_binding_set_and_save_safely(const String& uid,
                                     NfcBindType type,
                                     const String& key,
                                     const String& display,
                                     bool* was_playing_before = nullptr);

/**
 * @brief 安全删除一条绑定并保存到 nfc_map.txt。
 */
bool nfc_binding_remove_and_save_safely(const String& uid,
                                        bool* was_playing_before = nullptr,
                                        bool resume_playback_after_commit = false);

/**
 * @brief 安全删除所有绑定到同一个播放目标的 UID，并保存到 nfc_map.txt。
 *
 * 例如：删除所有“绑定到当前歌曲 audio_path”的卡片绑定。
 * removed_count 非空时返回本次实际删除条数。
 */
bool nfc_binding_remove_target_and_save_safely(NfcBindType type,
                                               const String& key,
                                               int* removed_count = nullptr,
                                               bool* was_playing_before = nullptr,
                                               bool resume_playback_after_commit = false);