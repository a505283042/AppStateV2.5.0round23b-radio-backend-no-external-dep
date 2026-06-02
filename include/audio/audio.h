#pragma once
#include <stdbool.h>

bool audio_init();
void audio_stop();
bool audio_play(const char* path); // 自动识别 .mp3 / .flac
bool audio_play_stream_mp3(const char* url); // HTTP MP3 流
void audio_loop();
bool audio_is_playing();

void     audio_set_volume(uint8_t percent);  // 0~100
uint8_t  audio_get_volume(void);
uint16_t audio_get_gain_q15(void);           // 0~32768 (Q15)

uint32_t audio_get_play_ms();
uint32_t audio_get_total_ms();   // 0 = unknown
void     audio_set_total_ms(uint32_t ms);
uint32_t audio_probe_total_ms(const char* path);
void     audio_reset_play_pos();