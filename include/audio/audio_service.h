#pragma once
#include <freertos/FreeRTOS.h>
#include <stdbool.h>
#include <freertos/task.h>
#include <stddef.h>
#include <stdint.h>
#include "storage/storage_types_v3.h"

enum class AudioOutputRoute : uint8_t;

enum class AudioNetworkStartPhase : uint8_t {
  Idle = 0,
  Connecting = 1,
  Playing = 2,
  Failed = 3,
  Cancelled = 4,
};

// 网络音频状态由 AudioTask 统一采集并发布。
// UI、Web 和播放器状态机只能读取该快照，禁止直接访问 WiFiClient 或网络解码器内部状态。
struct AudioNetworkStateSnapshot {
  uint32_t start_request_id = 0;
  AudioNetworkStartPhase start_phase = AudioNetworkStartPhase::Idle;
  bool active = false;
  bool source_open = false;
  bool transport_connected = false;
  bool waiting_for_data = false;
  bool reconnecting = false;
  bool eof = false;
  uint8_t reconnect_attempt = 0;
  uint32_t reconnect_delay_ms = 0;
  uint32_t available_bytes = 0;
  uint32_t cached_bytes = 0;
  uint32_t transport_available_bytes = 0;
  uint32_t buffer_capacity_bytes = 0;
  uint32_t reconnect_attempt_count = 0;
  uint32_t reconnect_success_count = 0;
  uint32_t last_data_ms = 0;
  uint32_t bitrate_kbps = 0;
  uint32_t sample_rate = 0;
  uint8_t channels = 0;
  char error[96] = {0};
};

// 启动音频专用任务（双核）：AudioTask 会独占 audio_* 接口并持续调用 audio_loop()
void audio_service_start(void);

// wait=true: 阻塞等待命令执行完成（用于 stop 后立刻读封面/扫描，避免 SD 并发）
bool audio_service_play(const char* path, bool wait);
bool audio_service_play_stream_mp3(const char* url, bool wait);
bool audio_service_play_stream_mp3_from_offset(const char* url, uint32_t start_offset, bool wait);

// 网络起播异步接口：只负责把请求加入 AudioTask 队列，不等待 HTTP 连接、响应头和预缓冲完成。
// out_request_id 用于播放器状态机匹配本次连接结果。
bool audio_service_play_stream_mp3_async(const char* url, uint32_t* out_request_id = nullptr);
bool audio_service_play_stream_mp3_auto_offset_async(const char* url, uint32_t* out_request_id = nullptr);
// NAS FLAC 使用 HTTP Range 可寻址音源。
bool audio_service_play_stream_flac_async(const char* url, uint32_t* out_request_id = nullptr);

bool audio_service_stop(bool wait);

// 播放状态（由 AudioTask 维护）
bool audio_service_is_playing(void);

// 获取 AudioTask 发布的网络音频只读快照。
bool audio_service_get_network_state(AudioNetworkStateSnapshot* out_snapshot);

// 暂停/恢复必须通过 AudioTask 串行执行，禁止调用线程直接修改音频运行状态。
bool audio_service_pause(bool wait = true);
bool audio_service_resume(bool wait = true);
bool audio_service_is_paused(void);

// 音频输出硬件控制统一由 AudioTask 执行。
bool audio_service_set_output_route(AudioOutputRoute route, bool wait = true);
bool audio_service_set_amp_mute(bool enabled, bool wait = true);
bool audio_service_set_amp_shutdown(bool enabled, bool wait = true);
bool audio_service_set_user_volume(uint8_t value, bool wait = true);
// 相对音量调整也必须由 AudioTask 串行执行，避免未知蓝牙音量被占位值误当成真实基准。
bool audio_service_step_user_volume(int delta, bool wait = true);

// 淡入淡出控制
float audio_service_get_fade_gain(void);

TaskHandle_t audio_service_get_task_handle(void);

// 播放期间的额外 SD 访问必须走 AudioTask 代办，避免跨任务触发 SdFat/SPI 事务断言。
// 读取类接口需要把结果所有权安全转交给调用方，因此仅支持 wait=true。
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
