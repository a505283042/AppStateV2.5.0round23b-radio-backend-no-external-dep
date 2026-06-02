#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "radio/radio_catalog.h"

enum class PlayerSourceType : uint8_t {
  NONE = 0,
  LOCAL_TRACK = 1,
  NET_RADIO = 2,
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
  String radio_state;      // idle / selected / unsupported / connecting / playing / paused / error / stopped
  String radio_error;
  String radio_stream_title;
  String radio_backend;
  uint32_t radio_bitrate = 0;
};

void player_source_reset();
void player_source_set_local_track(int track_idx);
void player_source_set_radio_stub(int radio_idx, const RadioItem& item, const String& state, const String& err);
void player_source_set_radio_status(bool active, const String& state, const String& err = String());
void player_source_set_radio_runtime(const String& backend, const String& stream_title, uint32_t bitrate, const String& state, bool active);
void player_source_clear_radio();
PlayerSourceState player_source_get();
const char* player_source_type_key(PlayerSourceType type);
