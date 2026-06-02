#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

bool audio_i2s_init(int bck, int ws, int dout, int sample_rate);
void audio_i2s_deinit();
size_t audio_i2s_write_frames(const int16_t* stereo_samples, size_t frames); // 返回实际写入帧数；返回SIZE_MAX表示错误
bool audio_i2s_set_sample_rate(int sample_rate); // 设置采样率
void     audio_i2s_reset_play_pos();
uint32_t audio_i2s_get_play_ms();
void     audio_i2s_zero_dma_buffer();  // 清空 DMA 缓冲区，消除底噪