#pragma once
#include <stdbool.h>
#include <SdFat.h>

bool audio_flac_start(SdFat& sd, const char* path);
void audio_flac_stop();
bool audio_flac_loop();
// 开播前预解码一段 FLAC PCM 到软件缓冲，不写入 I2S，避免静音预填充时把开头音频播掉。
uint32_t audio_flac_prime_pcm_ms(uint32_t target_ms, uint32_t max_chunks);
