#include "player_source.h"

namespace {
PlayerSourceState s_state{};
}

void player_source_reset() {
  s_state = PlayerSourceState{};
}

void player_source_set_local_track(int track_idx) {
  s_state.type = PlayerSourceType::LOCAL_TRACK;
  s_state.track_idx = track_idx;
  s_state.radio_idx = -1;
  s_state.radio_name = "";
  s_state.radio_url = "";
  s_state.radio_format = "";
  s_state.radio_region = "";
  s_state.radio_logo = "";
  s_state.radio_active = false;
  s_state.radio_state = "idle";
  s_state.radio_error = "";
  s_state.radio_stream_title = "";
  s_state.radio_backend = "";
  s_state.radio_bitrate = 0;

  s_state.net_track_idx = -1;
  s_state.net_track_title = "";
  s_state.net_track_url = "";
  s_state.net_track_format = "";
  s_state.net_track_artist = "";
  s_state.net_track_album = "";
  s_state.net_track_active = false;
  s_state.net_track_state = "idle";
  s_state.net_track_error = "";
}

void player_source_set_radio_stub(int radio_idx, const RadioItem& item, const String& state, const String& err) {
  s_state.type = PlayerSourceType::NET_RADIO;
  s_state.track_idx = -1;
  s_state.radio_idx = radio_idx;
  s_state.radio_name = item.name;
  s_state.radio_url = item.url;
  s_state.radio_format = item.format;
  s_state.radio_region = item.region;
  s_state.radio_logo = item.logo;
  s_state.radio_active = false;
  s_state.radio_state = state;
  s_state.radio_error = err;
  s_state.radio_stream_title = "";
  s_state.radio_backend = "";
  s_state.radio_bitrate = 0;

  s_state.net_track_idx = -1;
  s_state.net_track_title = "";
  s_state.net_track_url = "";
  s_state.net_track_format = "";
  s_state.net_track_artist = "";
  s_state.net_track_album = "";
  s_state.net_track_active = false;
  s_state.net_track_state = "idle";
  s_state.net_track_error = "";
}

void player_source_set_radio_runtime(const String& backend, const String& stream_title, uint32_t bitrate, const String& state, bool active) {
  if (s_state.type != PlayerSourceType::NET_RADIO) return;
  s_state.radio_backend = backend;
  if (stream_title.length()) s_state.radio_stream_title = stream_title;
  if (bitrate > 0) s_state.radio_bitrate = bitrate;
  s_state.radio_state = state;
  s_state.radio_active = active;
}

void player_source_set_radio_status(bool active, const String& state, const String& err) {
  if (s_state.type != PlayerSourceType::NET_RADIO) return;
  s_state.radio_active = active;
  s_state.radio_state = state;
  s_state.radio_error = err;
}

void player_source_clear_radio() {
  if (s_state.type == PlayerSourceType::NET_RADIO) {
    player_source_reset();
  }
}

void player_source_set_net_track_stub(int idx,
                                      const NetMusicItem& item,
                                      const String& url,
                                      const String& state,
                                      const String& err) {
  s_state.type = PlayerSourceType::NET_TRACK;

  s_state.track_idx = -1;

  s_state.radio_idx = -1;
  s_state.radio_name = "";
  s_state.radio_url = "";
  s_state.radio_format = "";
  s_state.radio_region = "";
  s_state.radio_logo = "";
  s_state.radio_active = false;
  s_state.radio_state = "idle";
  s_state.radio_error = "";
  s_state.radio_stream_title = "";
  s_state.radio_backend = "";
  s_state.radio_bitrate = 0;

  s_state.net_track_idx = idx;
  s_state.net_track_title = item.title;
  s_state.net_track_url = url;
  s_state.net_track_format = item.format;
  s_state.net_track_artist = item.artist;
  s_state.net_track_album = item.album;
  s_state.net_track_active = false;
  s_state.net_track_state = state;
  s_state.net_track_error = err;
}

void player_source_set_net_track_status(bool active,
                                        const String& state,
                                        const String& err) {
  if (s_state.type != PlayerSourceType::NET_TRACK) return;
  s_state.net_track_active = active;
  s_state.net_track_state = state;
  s_state.net_track_error = err;
}

void player_source_clear_net_track() {
  if (s_state.type == PlayerSourceType::NET_TRACK) {
    player_source_reset();
  }
}

PlayerSourceState player_source_get() {
  return s_state;
}

const char* player_source_type_key(PlayerSourceType type) {
  switch (type) {
    case PlayerSourceType::LOCAL_TRACK: return "track";
    case PlayerSourceType::NET_RADIO: return "radio";
    case PlayerSourceType::NET_TRACK: return "net_track";
    case PlayerSourceType::NONE:
    default: return "none";
  }
}
