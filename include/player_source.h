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

// 高频主循环只关心音源类型和活动标志，使用纯数值快照避免复制完整 String 状态。
struct PlayerSourceRuntimeState {
  PlayerSourceType type = PlayerSourceType::NONE;
  bool radio_active = false;
  bool net_track_active = false;
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
void player_source_set_radio_runtime(const char* backend,
                                     const String& station,
                                     const String& stream_title,
                                     uint32_t bitrate,
                                     const char* state,
                                     bool active,
                                     const String& err = String());
void player_source_clear_radio();

void player_source_set_net_track_stub(int idx,
                                      const NetMusicItem& item,
                                      const String& url,
                                      const String& state,
                                      const String& err);
void player_source_set_net_track_status(bool active,
                                        const String& state,
                                        const String& err = String());
                                        
// 起播后发现列表时长不可信时，更新当前 NAS 曲目的运行时有效时长。
void player_source_set_net_track_duration_ms(uint32_t duration_ms);
void player_source_clear_net_track();

PlayerSourceState player_source_get();
// 获取高频运行态，不复制状态中的 String。
PlayerSourceRuntimeState player_source_runtime_get();
// 仅获取当前音源类型，不复制状态中的 String，供只判断类型的路径使用。
PlayerSourceType player_source_type_get();
const char* player_source_type_key(PlayerSourceType type);

