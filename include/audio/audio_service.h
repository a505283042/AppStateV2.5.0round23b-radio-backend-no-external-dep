#pragma once
#include <freertos/FreeRTOS.h>
#include <stdbool.h>
#include <freertos/task.h>
#include <stddef.h>
#include <stdint.h>
#include "storage/storage_types_v3.h"

// 启动音频专用任务（双核）：AudioTask 会独占 audio_* 接口并持续调用 audio_loop()
void audio_service_start(void);

// wait=true: 阻塞等待命令执行完成（用于 stop 后立刻读封面/扫描，避免 SD 并发）
bool audio_service_play(const char* path, bool wait);
bool audio_service_play_stream_mp3(const char* url, bool wait);
bool audio_service_stop(bool wait);

// 播放状态（由 AudioTask 维护）
bool audio_service_is_playing(void);

// 暂停控制接口
void audio_service_pause(void);
void audio_service_resume(void);
bool audio_service_is_paused(void);

// 淡入淡出控制
float audio_service_get_fade_gain(void);

TaskHandle_t audio_service_get_task_handle(void);

// 播放期间的额外 SD 访问必须走 AudioTask 代办，避免跨任务触发 SdFat/SPI 事务断言。
bool audio_service_fetch_lyrics(const char* path, char** out_text, size_t* out_len, bool wait = true);
bool audio_service_fetch_total_ms(const char* path, uint32_t* out_total_ms, bool wait = true);
bool audio_service_fetch_cover(CoverSource cover_source,
                               const char* audio_path,
                               const char* cover_path,
                               uint32_t cover_offset,
                               uint32_t cover_size,
                               uint8_t** out_buf,
                               size_t* out_len,
                               bool* out_is_png,
                               bool wait = true);
