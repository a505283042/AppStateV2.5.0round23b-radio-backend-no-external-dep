#pragma once

#include <Arduino.h>

// 网页刷新速度档位：影响 /api/status 的建议轮询间隔。
enum class WebRefreshPreset : uint8_t {
  POWER_SAVE = 0,
  BALANCED = 1,
  SMOOTH = 2,
};

// 歌词更新策略：影响“接近下一次轮询时是否等轮询更新”的阈值。
enum class WebLyricSyncMode : uint8_t {
  PRECISE = 0,
  BALANCED = 1,
  FOLLOW_POLL = 2,
};

struct WebRuntimeSettings {
  // 只保留两个和轮询/歌词更新直接相关、用户容易理解的设置：
  WebRefreshPreset refresh_preset = WebRefreshPreset::BALANCED;
  WebLyricSyncMode lyric_sync_mode = WebLyricSyncMode::BALANCED;

  // 其它更有感知的网页显示设置：
  bool show_next_lyric = true;
  bool show_cover = true;
  bool web_cover_spin = true;
  bool show_wifi_info = true;
};

// 启动时优先从 NVS 加载网页运行设置；若不存在则兼容导入旧版 SD 配置；都没有则使用默认值。
bool web_settings_load();
// 将当前网页运行设置保存到 NVS（避免播放中写 SD 导致网页设置保存不稳定）。
bool web_settings_save();
// 获取当前网页运行设置快照。
const WebRuntimeSettings& web_settings_get();
// 更新当前网页运行设置（不自动保存）。
void web_settings_set(const WebRuntimeSettings& s);

// 机器可读 key / 中文标签 / 由档位映射出的实际参数。
const char* web_refresh_preset_key(WebRefreshPreset p);
const char* web_refresh_preset_label(WebRefreshPreset p);
uint32_t web_refresh_preset_poll_ms(WebRefreshPreset p);

const char* web_lyric_sync_mode_key(WebLyricSyncMode m);
const char* web_lyric_sync_mode_label(WebLyricSyncMode m);
uint32_t web_lyric_sync_mode_threshold_ms(WebLyricSyncMode m);
