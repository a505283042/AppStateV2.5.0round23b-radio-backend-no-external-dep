#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <SdFat.h>

#include "audio/audio.h"

bool audio_flac_start(SdFat& sd, const char* path);
bool audio_flac_start_url(const char* url, uint32_t operation_id);
void audio_flac_stop();
bool audio_flac_loop();
AudioPlaybackEndReason audio_flac_get_end_reason();
bool audio_flac_is_active();
bool audio_flac_is_network_source();
uint32_t audio_flac_get_sample_rate();
uint8_t audio_flac_get_channels();
uint32_t audio_flac_get_total_ms();
bool audio_flac_is_seekable();
bool audio_flac_seek_ms(uint32_t target_ms, uint32_t* out_actual_ms);
const char* audio_flac_get_last_error();
// 开播前预解码一段 FLAC PCM 到软件缓冲，不写入 I2S，避免静音预填充时把开头音频播掉。
uint32_t audio_flac_prime_pcm_ms(uint32_t target_ms, uint32_t max_chunks);
