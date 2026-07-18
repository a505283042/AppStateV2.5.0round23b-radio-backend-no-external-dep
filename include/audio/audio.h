#pragma once
#include <stdbool.h>
#include <stdint.h>

// 本地音频结束原因。只由 AudioTask 写入，播放器通过快照读取。
enum class AudioPlaybackEndReason : uint8_t {
  None = 0,
  NaturalEof,
  SourceIoError,
  DecodeError,
  OutputError,
  Stopped,
};

struct AudioPlaybackEndState {
  uint32_t serial = 0;
  AudioPlaybackEndReason reason = AudioPlaybackEndReason::None;
  uint32_t play_ms = 0;
  uint32_t total_ms = 0;
  uint32_t ended_at_ms = 0;
};

// 音频核心运行态由多个任务读取，使用纯数值快照保证总时长、音量和增益来自同一版本。
struct AudioRuntimeSnapshot {
  uint32_t total_ms = 0;
  uint8_t volume_percent = 80;
  uint16_t gain_q15 = 26214;
  uint32_t revision = 0;
};

bool audio_init();
void audio_stop();
bool audio_play(const char* path); // 自动识别 .mp3 / .flac
bool audio_play_stream_mp3(const char* url, uint32_t operation_id); // HTTP MP3 流
bool audio_play_stream_mp3_from_offset(const char* url, uint32_t start_offset, uint32_t operation_id); // HTTP MP3 Range 跳过 ID3 起播
void audio_loop();
bool audio_is_playing();
AudioPlaybackEndState audio_get_last_end_state();
const char* audio_playback_end_reason_label(AudioPlaybackEndReason reason);

void     audio_set_volume(uint8_t percent);  // 0~100
uint8_t  audio_get_volume(void);
uint16_t audio_get_gain_q15(void);           // 0~32768 (Q15)

uint32_t audio_get_play_ms();
AudioRuntimeSnapshot audio_runtime_snapshot_get();
uint32_t audio_get_total_ms();   // 0 = unknown
void     audio_set_total_ms(uint32_t ms);
uint32_t audio_probe_total_ms(const char* path);
void     audio_reset_play_pos();
// 当前解码器支持时，开播前预解码一段 PCM 到软件缓冲；返回已缓存的音频时长。
uint32_t audio_prime_pcm_ms(uint32_t target_ms, uint32_t max_chunks);