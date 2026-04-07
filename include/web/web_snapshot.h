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
  String app_state;         // 机器可读状态 key，例如 player / rescanning
  String app_state_label;   // 中文显示文案
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
  String mode;              // 机器可读模式 key，例如 all_seq / artist_rnd
  String mode_label;        // 中文显示文案
  String view;              // 当前网页/设备主视图 key，例如 info / rotate
  String view_label;        // 当前主视图中文文案
  int display_pos = -1;
  int display_total = 0;
  int current_group_idx = -1;
  String net_mode;
  String ip;
  String wifi_name;         // 当前连接的 Wi‑Fi 名称；AP 模式下显示热点名
  String hostname;
  String wifi_source;        // 调试字段，当前 Wi‑Fi 来源：config_file / ap_fallback
  bool can_cancel_scan = false;
  String scan_action_label;

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
  String cover_url;

  // 网络电台 / 播放源摘要（round16 scaffold）
  String source_type;
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

  // 建议网页下一次刷新等待多久（毫秒），用于自适应轮询。
  uint32_t next_poll_ms = 0;
};

/** 采样当前播放器状态，供网页 API 返回。 */
WebPlayerSnapshot web_snapshot_capture();
