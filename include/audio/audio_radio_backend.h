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
