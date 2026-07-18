#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "radio/radio_catalog.h"
#include "net_music/net_music_catalog.h"

enum class PlayerSourceType : uint8_t {
  NONE = 0,
  LOCAL_TRACK = 1,
  NET_RADIO = 2,
  NET_TRACK = 3,
};

struct PlayerSourceState {
  PlayerSourceType type = PlayerSourceType::NONE;
  int track_idx = -1;
  int radio_idx = -1;
  String radio_name;
  String radio_url;
  String radio_format;
  String radio_region;
  String radio_logo;
  bool radio_active = false;
  String radio_state;      // idle / selected / unsupported / connecting / reconnecting / playing / paused / error / stopped
  String radio_error;
  String radio_stream_title;
  String radio_backend;
  uint32_t radio_bitrate = 0;

  int net_track_idx = -1;
  String net_track_title;
  String net_track_url;
  String net_track_format;
  String net_track_artist;
  String net_track_album;
  uint32_t net_track_duration_ms = 0;
  bool net_track_active = false;
  String net_track_state;   // idle / connecting / playing / paused / error / stopped
  String net_track_error;
};

void player_source_reset();
void player_source_set_local_track(int track_idx);
void player_source_set_radio_stub(int radio_idx, const RadioItem& item, const String& state, const String& err);
void player_source_set_radio_status(bool active, const String& state, const String& err = String());
void player_source_set_radio_runtime(const String& backend, const String& stream_title, uint32_t bitrate, const String& state, bool active);
void player_source_clear_radio();

void player_source_set_net_track_stub(int idx,
                                      const NetMusicItem& item,
                                      const String& url,
                                      const String& state,
                                      const String& err);
void player_source_set_net_track_status(bool active,
                                        const String& state,
                                        const String& err = String());
void player_source_clear_net_track();

PlayerSourceState player_source_get();
// 仅获取当前音源类型，不复制状态中的 String，供高频 UI 绘制使用。
PlayerSourceType player_source_type_get();
const char* player_source_type_key(PlayerSourceType type);

