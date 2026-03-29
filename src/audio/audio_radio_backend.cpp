#include "audio/audio_radio_backend.h"

#include "audio/audio.h"
#include "audio/audio_service.h"
#include "audio/audio_mp3.h"
#include "audio/audio_mp3_source_audiotools.h"
#include "utils/log.h"

namespace {
RadioBackendStatus s_status{};
String s_station;
String s_url;
String s_region;
bool s_inited = false;

}

bool audio_radio_backend_begin() {
  if (s_inited) return true;
  s_inited = true;
  return true;
}

bool audio_radio_backend_start(const RadioItem& item) {
  audio_radio_backend_begin();
  s_station = item.name;
  s_url = item.url;
  s_region = item.region;
  s_status = RadioBackendStatus{};
  s_status.supported = true;
  s_status.station = item.name;
  s_status.info = item.region;
  s_status.connecting = true;
  s_status.active = true;
  const bool ok = audio_service_play_stream_mp3(item.url.c_str(), true);
  if (!ok) {
    s_status.active = false;
    s_status.connecting = false;
    s_status.running = false;
    s_status.error = String("stream_start_failed");
    return false;
  }
  return true;
}

void audio_radio_backend_stop() {
  audio_service_stop(true);
  s_status = RadioBackendStatus{};
  s_status.supported = true;
}

void audio_radio_backend_loop() {
  const bool mp3_active = audio_mp3_is_active() && audio_mp3_is_stream_source();
  const bool paused = audio_service_is_paused();
  const bool connected = audio_mp3_audiotools_source_connected();
  const int avail = audio_mp3_audiotools_source_available();

  s_status.supported = true;
  s_status.active = mp3_active || paused;
  s_status.paused = paused;
  s_status.connecting = s_status.active && connected && avail == 0;
  s_status.running = s_status.active && !s_status.paused && connected;

  s_status.bitrate = audio_mp3_get_bitrate_kbps();
  s_status.sample_rate = audio_mp3_get_sample_rate();
  s_status.channels = audio_mp3_get_channels();

  s_status.inbuf_filled = avail > 0 ? (uint32_t)avail : 0;
  s_status.inbuf_size = 0;

  if (s_station.length()) s_status.station = s_station;
  if (s_region.length()) s_status.info = s_region;

  const char* err = audio_mp3_get_last_error();
  s_status.error = (err && *err) ? String(err) : String();
}

bool audio_radio_backend_is_active() { return audio_service_is_playing() || audio_service_is_paused(); }
bool audio_radio_backend_is_paused() { return audio_service_is_paused(); }

bool audio_radio_backend_toggle_pause() {
  if (!audio_radio_backend_is_active()) return false;
  if (audio_service_is_paused()) audio_service_resume(); else audio_service_pause();
  return true;
}

RadioBackendStatus audio_radio_backend_get_status() {
  return s_status;
}

const char* audio_radio_backend_name() {
  return "audiotools-urlstream";
}
