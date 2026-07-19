#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <SdFat.h>

#include "audio/audio.h"
#include "audio/audio_mp3_source.h"

bool audio_mp3_start_file(SdFat& sd, const char* path);
bool audio_mp3_start_url(const char* url, uint32_t operation_id);
bool audio_mp3_start_url_from_offset(const char* url, uint32_t start_offset, uint32_t operation_id);
bool audio_mp3_is_seekable();
bool audio_mp3_seek_ms(uint32_t target_ms, uint32_t total_ms, uint32_t* out_actual_ms);
bool audio_mp3_start_source(const AudioMp3Source& source, const char* debug_name = nullptr);

void audio_mp3_stop();
bool audio_mp3_loop(); // 解码一段并输出，返回是否还在播放
AudioPlaybackEndReason audio_mp3_get_end_reason();

bool audio_mp3_is_active();
bool audio_mp3_is_stream_source();
uint32_t audio_mp3_get_sample_rate();
uint8_t audio_mp3_get_channels();
uint32_t audio_mp3_get_bitrate_kbps();
const char* audio_mp3_get_last_error();

