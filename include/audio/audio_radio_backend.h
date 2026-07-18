#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "radio/radio_catalog.h"

struct RadioBackendStatus {
  bool supported = true;
  bool active = false;
  bool paused = false;
  bool connecting = false;
  bool running = false;
  bool retrying = false;         // 正在等待或执行自动重连
  uint8_t retry_attempt = 0;     // 当前连续故障周期内已发起的重连次数
  uint8_t retry_limit = 0;       // 本轮允许的最大重连次数
  uint32_t retry_in_ms = 0;      // 距离下一次重连的剩余时间
  uint32_t bitrate = 0;
  uint32_t sample_rate = 0;
  uint8_t channels = 0;
  uint32_t inbuf_filled = 0;
  uint32_t inbuf_size = 0;
  String station;
  String stream_title;
  String info;
  String error;
};

bool audio_radio_backend_begin();
bool audio_radio_backend_start(const RadioItem& item);
void audio_radio_backend_stop();
void audio_radio_backend_loop();
bool audio_radio_backend_is_active();
bool audio_radio_backend_is_paused();
bool audio_radio_backend_toggle_pause();
RadioBackendStatus audio_radio_backend_get_status();
const char* audio_radio_backend_name();
