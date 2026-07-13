#pragma once

#include <Arduino.h>

/**
 * @brief 网页控制只读状态快照。
 *
 * 这个结构体的作用是把“网页真正关心的播放器状态”整理成一份轻量摘要，
 * 避免 web 模块直接依赖 UI 内部状态或各模块的零散全局变量。
 */
struct WebPlayerSnapshot {
  bool ok = false;
  const char* app_state = "unknown";       // 静态状态 key，避免高频轮询反复分配 String
  const char* app_state_label = "未知";     // 静态中文文案
  bool rescanning = false;
  bool is_playing = false;
  bool is_paused = false;
  int track_idx = -1;
  String title;
  String artist;
  String album;
  uint32_t play_ms = 0;
  uint32_t total_ms = 0;
  uint8_t volume = 0;
  const char* mode = "unknown";            // 静态模式 key
  const char* mode_label = "未知";          // 静态模式文案
  const char* view = "unknown";            // 静态视图 key
  const char* view_label = "未知视图";      // 静态视图文案
  int display_pos = -1;
  int display_total = 0;
  int current_group_idx = -1;
  String net_mode;
  String ip;
  String wifi_name;         // 当前连接的 Wi‑Fi 名称；AP 模式下显示热点名
  String hostname;
  String wifi_source;        // 调试字段，当前 Wi‑Fi 来源：config_file / ap_fallback
  bool can_cancel_scan = false;
  const char* scan_action_label = "开始重扫";

  // 第二步网页增强：歌词摘要与封面状态
  bool has_lyrics = false;
  bool lyrics_loading = false;
  String current_lyric;
  String next_lyric;
  bool show_next_lyric = true;
  bool show_cover = true;
  bool web_cover_spin = true;
  String following_lyric;
  uint32_t current_lyric_start_ms = 0;
  uint32_t next_lyric_start_ms = 0;
  uint32_t following_lyric_start_ms = 0;
  bool has_cover = false;
  bool cover_loading = false;
  bool cover_ready_for_web = false;
  String cover_rev;   // 当前封面的版本标识，封面源变化时必须变化
  String cover_url;

  // 网络电台 / 播放源摘要（round16 scaffold）
  const char* source_type = "none";         // player_source_type_key() 返回静态字符串
  bool radio_active = false;
  int radio_idx = -1;
  String radio_name;
  String radio_format;
  String radio_region;
  String radio_state;
  String radio_error;
  String radio_stream_title;
  String radio_backend;
  uint32_t radio_bitrate = 0;

  bool net_track_active = false;
  int net_track_idx = -1;
  String net_track_title;
  String net_track_url;
  String net_track_format;
  String net_track_artist;
  String net_track_album;
  uint32_t net_track_duration_ms = 0;
  String net_track_state;
  String net_track_error;

  // 建议网页下一次刷新等待多久（毫秒），用于自适应轮询。
  uint32_t next_poll_ms = 0;
};

/**
 * @brief 采样当前播放器状态到调用方提供的对象。
 *
 * 高频接口应复用同一个对象，使内部 String 保留容量，避免每次轮询反复申请和释放。
 */
void web_snapshot_capture_into(WebPlayerSnapshot& snap);

/** 兼容低频调用：创建并返回一份独立快照。 */
WebPlayerSnapshot web_snapshot_capture();

/** 生成电台封面的版本标识。 */
String web_make_radio_cover_rev(int radio_idx, const String& logo);
