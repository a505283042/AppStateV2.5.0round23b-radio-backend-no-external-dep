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

// WS2812 状态灯亮度档位。
enum class StatusLedBrightness : uint8_t {
  Low = 0,
  Medium = 1,
  High = 2,
};

struct WebRuntimeSettings {
  // 只保留两个和轮询/歌词更新直接相关、用户容易理解的设置：
  WebRefreshPreset refresh_preset = WebRefreshPreset::BALANCED;
  WebLyricSyncMode lyric_sync_mode = WebLyricSyncMode::BALANCED;

  // 其它更有感知的网页显示设置：
  bool show_next_lyric = true;
  bool show_cover = true;
  bool web_cover_spin = true;

  // WiFi 总开关：保存到 NVS。
  // false 时，下次开机不自动连接 STA，也不启动 AP fallback。
  bool wifi_enabled = true;

  // 是否在屏幕 / Web 状态里显示 WiFi 信息。
  // 这只是显示开关，不影响 WiFi 本身。
  bool show_wifi_info = true;

  // HALL_OUT 霍尔输入总开关。
  // 关闭后 GPIO9 仍保持输入，但不再控制播放 / 暂停。
  bool hall_control_enabled = true;

  // TC118S 电磁铁动作总开关。
  // 关闭后播放键不再输出电磁铁短脉冲，驱动层仍保持停止态保护。
  bool solenoid_enabled = true;

  // GPIO3 WS2812 播放状态灯。
  bool status_led_enabled = true;
  StatusLedBrightness status_led_brightness = StatusLedBrightness::Medium;

};

// 启动时从 NVS 加载网页运行设置；没有则使用默认值。
bool web_settings_load();
// 将当前网页运行设置保存到 NVS（避免播放中写 SD 导致网页设置保存不稳定）。
bool web_settings_save();
// 获取当前网页运行设置快照。返回值已在内部加锁复制，可安全跨任务读取。
WebRuntimeSettings web_settings_get();
// 更新当前网页运行设置（不自动保存）；内容有变化时只标记为待保存。
void web_settings_set(const WebRuntimeSettings& s);
// 当前网页设置是否有待保存改动。
bool web_settings_is_dirty();
// 仅在有待保存改动时写入 NVS；没有改动时直接返回 true。
bool web_settings_save_if_dirty();

// 机器可读 key / 中文标签 / 由档位映射出的实际参数。
const char* web_refresh_preset_key(WebRefreshPreset p);
const char* web_refresh_preset_label(WebRefreshPreset p);
uint32_t web_refresh_preset_poll_ms(WebRefreshPreset p);

const char* web_lyric_sync_mode_key(WebLyricSyncMode m);
const char* web_lyric_sync_mode_label(WebLyricSyncMode m);
uint32_t web_lyric_sync_mode_threshold_ms(WebLyricSyncMode m);

const char* status_led_brightness_key(StatusLedBrightness value);
const char* status_led_brightness_label(StatusLedBrightness value);
uint8_t status_led_brightness_value(StatusLedBrightness value);
