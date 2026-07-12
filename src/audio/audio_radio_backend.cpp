#include "audio/audio_radio_backend.h"

#include "audio/audio_service.h"
#include "utils/log.h"

namespace {
RadioBackendStatus s_status{};
String s_station;
String s_url;
String s_region;
bool s_inited = false;
uint32_t s_pending_request_id = 0;
uint32_t s_pending_started_ms = 0;
static constexpr uint32_t kRadioStartTimeoutMs = 25000;

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

  s_pending_request_id = 0;
  s_pending_started_ms = millis();
  const bool queued = audio_service_play_stream_mp3_async(item.url.c_str(), &s_pending_request_id);
  if (!queued || s_pending_request_id == 0) {
    s_status.active = false;
    s_status.connecting = false;
    s_status.running = false;
    s_status.error = String("queue_failed");
    s_pending_request_id = 0;
    s_pending_started_ms = 0;
    return false;
  }

  LOGI("[电台] 异步起播请求已入队：请求=%lu 名称=%s",
       (unsigned long)s_pending_request_id,
       item.name.c_str());
  return true;
}

void audio_radio_backend_stop() {
  s_pending_request_id = 0;
  s_pending_started_ms = 0;
  audio_service_stop(true);
  s_status = RadioBackendStatus{};
  s_status.supported = true;
}

void audio_radio_backend_loop() {
  AudioNetworkStateSnapshot network{};
  (void)audio_service_get_network_state(&network);

  const bool paused = audio_service_is_paused();

  s_status.supported = true;

  if (s_pending_request_id != 0) {
    if (millis() - s_pending_started_ms >= kRadioStartTimeoutMs) {
      (void)audio_service_stop(false);
      s_status.active = false;
      s_status.paused = false;
      s_status.connecting = false;
      s_status.running = false;
      s_status.error = String("startup_timeout");
      LOGW("[电台] 异步起播超时：请求=%lu", (unsigned long)s_pending_request_id);
      s_pending_request_id = 0;
      s_pending_started_ms = 0;
      return;
    }

    // 请求尚未被 AudioTask 取出时，快照仍可能是上一条请求；保持“连接中”。
    if (network.start_request_id != s_pending_request_id) {
      s_status.active = true;
      s_status.paused = false;
      s_status.connecting = true;
      s_status.running = false;
      return;
    }

    if (network.start_phase == AudioNetworkStartPhase::Connecting) {
      s_status.active = true;
      s_status.paused = false;
      s_status.connecting = true;
      s_status.running = false;
      s_status.error = String();
      return;
    }

    if (network.start_phase == AudioNetworkStartPhase::Failed ||
        network.start_phase == AudioNetworkStartPhase::Cancelled) {
      s_status.active = false;
      s_status.paused = false;
      s_status.connecting = false;
      s_status.running = false;
      s_status.error = network.error[0] ? String(network.error) : String("stream_start_failed");
      LOGW("[电台] 异步起播失败：请求=%lu 原因=%s",
           (unsigned long)s_pending_request_id,
           s_status.error.c_str());
      s_pending_request_id = 0;
      s_pending_started_ms = 0;
      return;
    }

    if (network.start_phase == AudioNetworkStartPhase::Playing && network.active) {
      LOGI("[电台] 异步起播完成：请求=%lu", (unsigned long)s_pending_request_id);
      s_pending_request_id = 0;
      s_pending_started_ms = 0;
    }
  }

  s_status.active = network.active;
  s_status.paused = network.active && paused;
  s_status.connecting = s_status.active &&
                        !s_status.paused &&
                        network.source_open &&
                        network.transport_connected &&
                        network.waiting_for_data;
  s_status.running = s_status.active &&
                     !s_status.paused &&
                     network.source_open &&
                     network.transport_connected &&
                     !network.eof;

  s_status.bitrate = network.bitrate_kbps;
  s_status.sample_rate = network.sample_rate;
  s_status.channels = network.channels;

  s_status.inbuf_filled = network.available_bytes;
  s_status.inbuf_size = 0;

  if (s_station.length()) s_status.station = s_station;
  if (s_region.length()) s_status.info = s_region;

  s_status.error = network.error[0] ? String(network.error) : String();
}

bool audio_radio_backend_is_active() {
  return s_status.active;
}

bool audio_radio_backend_is_paused() {
  return audio_radio_backend_is_active() && audio_service_is_paused();
}

bool audio_radio_backend_toggle_pause() {
  if (!audio_radio_backend_is_active()) return false;
  return audio_service_is_paused()
      ? audio_service_resume(true)
      : audio_service_pause(true);
}

RadioBackendStatus audio_radio_backend_get_status() {
  return s_status;
}

const char* audio_radio_backend_name() {
  return "audiotools-urlstream";
}
