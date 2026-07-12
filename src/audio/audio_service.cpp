#include <Arduino.h>
#include <WiFi.h>
#include <string.h>
#include <stdlib.h>
#include <new>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <esp32-hal-psram.h>
#include <SdFat.h>

#include "audio/audio_service.h"
#include "audio/audio.h"
#include "audio/audio_file.h"
#include "audio/audio_mp3.h"
#include "audio/audio_mp3_source_audiotools.h"
#include "audio/audio_i2s.h"
#include "audio/audio_output_route.h"
#include "storage/storage_io.h"
#include "hal/board_hw_control.h"
#include "audio/audio_output_route.h"
#include "utils/log.h"

extern SdFat sd;

#ifndef AUDIO_TASK_CORE
#define AUDIO_TASK_CORE 0   // Arduino loopTask 默认在 core1；把音频钉到 core0 更稳
#endif

#ifndef AUDIO_TASK_PRIO
#define AUDIO_TASK_PRIO 5
#endif

static constexpr uint32_t kAudioTaskStackBytes = 10240; // 音频任务栈大小

#ifndef AUDIO_CMD_PATH_MAX
#define AUDIO_CMD_PATH_MAX 512
#endif

#ifndef AUDIO_AUX_READ_CHUNK
#define AUDIO_AUX_READ_CHUNK 2048
#endif

#ifndef AUDIO_CMD_QUEUE_LEN
#define AUDIO_CMD_QUEUE_LEN 4
#endif

enum AudioCmdType : uint8_t {
  CMD_PLAY = 1,
  CMD_STOP = 2,
  CMD_FETCH_LYRICS = 3,
  CMD_FETCH_COVER = 4,
  CMD_FETCH_TOTAL_MS = 5,
  CMD_PLAY_STREAM_MP3 = 6,
  CMD_PAUSE = 7,
  CMD_RESUME = 8,
  CMD_SET_OUTPUT_ROUTE = 9,
  CMD_SET_AMP_MUTE = 10,
  CMD_SET_AMP_SHUTDOWN = 11,
  CMD_SET_USER_VOLUME = 12,
};

struct AudioRequest {
  AudioCmdType type;
  char path[AUDIO_CMD_PATH_MAX];       // play / lyrics / embedded cover audio path
  char cover_path[AUDIO_CMD_PATH_MAX]; // fallback cover file path

  CoverSource cover_source = COVER_NONE;
  uint32_t cover_offset = 0;
  uint32_t cover_size = 0;
  uint32_t stream_start_offset = 0; // HTTP MP3 Range 起播偏移，0=普通起播
  uint32_t network_operation_id = 0;
  bool stream_probe_audio_offset = false;
  AudioOutputRoute output_route = AudioOutputRoute::Speaker;
  bool enabled = false;
  uint8_t volume = 0;

  // 请求结果只保存在请求对象内部，AudioTask 不再写调用方栈地址。
  bool success = false;
  uint32_t result_total_ms = 0;
  char* result_text = nullptr;
  size_t result_text_len = 0;
  uint8_t* result_buf = nullptr;
  size_t result_buf_len = 0;
  bool result_is_png = false;

  // wait=true 时创建独立完成信号量，避免复用任务通知收到过期 ACK。
  SemaphoreHandle_t done = nullptr;
  uint32_t request_id = 0;
  uint8_t refs = 1; // 调用方持有一个引用，入队成功后 AudioTask 再持有一个引用
};

static QueueHandle_t s_q = nullptr;
static TaskHandle_t  s_task = nullptr;
static portMUX_TYPE s_request_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_next_request_id = 1;

// 以下运行状态只允许 AudioTask 写入。其他任务只能读取 s_state_snapshot。
static bool s_task_ready = false;
static bool s_task_playing = false;
static bool s_task_paused = false;
static float s_task_fade_gain = 1.0f;
static float s_task_last_fade_gain = 1.0f;

struct AudioStateSnapshot {
  bool ready = false;
  bool playing = false;
  bool paused = false;
  float fade_gain = 1.0f;
};

static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;
static AudioStateSnapshot s_state_snapshot;
static AudioNetworkStateSnapshot s_network_state_snapshot;
static uint32_t s_task_network_request_id = 0;
static AudioNetworkStartPhase s_task_network_start_phase = AudioNetworkStartPhase::Idle;
static char s_task_network_error[96] = {0};

#define PAUSE_FADE_STEP   0.05f   // 暂停时淡出，保持柔和
#define PLAY_FADE_STEP    0.12f   // 开播/切歌时淡入，加快恢复正常音量
#define PLAY_START_GAIN   0.35f   // 新歌起播初始增益，别从 0 开始
// FLAC 某些歌曲解码会短时间超过实时长度。
// 注意：不能在功放静音时直接预填 I2S DMA，否则 I2S 硬件会把开头音频播放掉；
// 这里改为“软件预解码 PCM 到 RAM”，真正开声后再写入 I2S。
#define PLAY_PREFILL_TARGET_MS 160
#define PLAY_PREFILL_MAX_LOOPS 8
#define STOP_FADE_MAX_ITERS 8
#define STOP_FADE_STEP    0.20f

static inline bool detect_png_from_buffer(const uint8_t* b, size_t len) {
  return (len >= 8 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G' &&
          b[4] == 0x0D && b[5] == 0x0A && b[6] == 0x1A && b[7] == 0x0A);
}

static inline bool detect_jpg_from_buffer(const uint8_t* b, size_t len) {
  return (len >= 2 && b[0] == 0xFF && b[1] == 0xD8);
}

static constexpr size_t kInternalFallbackMaxBytes = 32 * 1024;

static void* alloc_aux_buffer(size_t size, const char* tag) {
  if (size == 0) return nullptr;

  void* p = ps_malloc(size);
  if (p) return p;

  if (size > kInternalFallbackMaxBytes) {
    LOGW("[音频] %s PSRAM 分配失败，禁止回落内部RAM size=%u",
         tag ? tag : "buffer",
         (unsigned)size);
    return nullptr;
  }

  return malloc(size);
}

static void audio_request_retain(AudioRequest* request)
{
  if (!request) return;
  portENTER_CRITICAL(&s_request_mux);
  ++request->refs;
  portEXIT_CRITICAL(&s_request_mux);
}

static void audio_request_destroy(AudioRequest* request)
{
  if (!request) return;

  // 调用方成功接管结果后会把对应指针清空；未接管的结果在这里兜底释放。
  if (request->result_text) {
    free(request->result_text);
    request->result_text = nullptr;
  }
  if (request->result_buf) {
    free(request->result_buf);
    request->result_buf = nullptr;
  }
  if (request->done) {
    vSemaphoreDelete(request->done);
    request->done = nullptr;
  }
  delete request;
}

static void audio_request_release(AudioRequest* request)
{
  if (!request) return;

  bool should_destroy = false;
  portENTER_CRITICAL(&s_request_mux);
  if (request->refs > 0) {
    --request->refs;
    should_destroy = (request->refs == 0);
  }
  portEXIT_CRITICAL(&s_request_mux);

  if (should_destroy) {
    audio_request_destroy(request);
  }
}

static AudioRequest* audio_request_new(bool wait)
{
  AudioRequest* request = new (std::nothrow) AudioRequest();
  if (!request) {
    LOGE("[音频] 分配请求对象失败：size=%u", (unsigned)sizeof(AudioRequest));
    return nullptr;
  }

  portENTER_CRITICAL(&s_request_mux);
  request->request_id = s_next_request_id++;
  if (s_next_request_id == 0) {
    s_next_request_id = 1;
  }
  portEXIT_CRITICAL(&s_request_mux);

  if (wait) {
    request->done = xSemaphoreCreateBinary();
    if (!request->done) {
      LOGE("[音频] 创建请求完成信号量失败：请求=%lu", (unsigned long)request->request_id);
      delete request;
      return nullptr;
    }
  }

  return request;
}

static void audio_task_set_network_start_state(uint32_t request_id,
                                               AudioNetworkStartPhase phase,
                                               const char* error = nullptr)
{
  s_task_network_request_id = request_id;
  s_task_network_start_phase = phase;
  s_task_network_error[0] = '\0';
  if (error && *error) {
    strncpy(s_task_network_error, error, sizeof(s_task_network_error) - 1);
    s_task_network_error[sizeof(s_task_network_error) - 1] = '\0';
  }
}

static void audio_task_publish_state()
{
  AudioNetworkStateSnapshot network_snapshot{};
  AudioMp3HttpSourceSnapshot source_snapshot{};
  (void)audio_mp3_audiotools_source_get_snapshot(&source_snapshot);

  network_snapshot.start_request_id = s_task_network_request_id;
  network_snapshot.start_phase = s_task_network_start_phase;
  network_snapshot.active = audio_mp3_is_active() && audio_mp3_is_stream_source();
  network_snapshot.source_open = source_snapshot.open;
  network_snapshot.transport_connected = source_snapshot.transport_connected;
  network_snapshot.waiting_for_data = source_snapshot.waiting_for_data;
  network_snapshot.eof = source_snapshot.eof;
  network_snapshot.available_bytes = source_snapshot.available_bytes;
  network_snapshot.last_data_ms = source_snapshot.last_data_ms;

  if (network_snapshot.active) {
    network_snapshot.bitrate_kbps = audio_mp3_get_bitrate_kbps();
    network_snapshot.sample_rate = audio_mp3_get_sample_rate();
    network_snapshot.channels = audio_mp3_get_channels();
  }

  const char* error = s_task_network_error[0]
      ? s_task_network_error
      : audio_mp3_get_last_error();
  if (error && *error) {
    strncpy(network_snapshot.error, error, sizeof(network_snapshot.error) - 1);
    network_snapshot.error[sizeof(network_snapshot.error) - 1] = '\0';
  }

  portENTER_CRITICAL(&s_state_mux);
  s_state_snapshot.ready = s_task_ready;
  s_state_snapshot.playing = s_task_playing;
  s_state_snapshot.paused = s_task_paused;
  s_state_snapshot.fade_gain = s_task_fade_gain;
  s_network_state_snapshot = network_snapshot;
  portEXIT_CRITICAL(&s_state_mux);
}

static AudioStateSnapshot audio_service_read_state_snapshot()
{
  AudioStateSnapshot snapshot;
  portENTER_CRITICAL(&s_state_mux);
  snapshot = s_state_snapshot;
  portEXIT_CRITICAL(&s_state_mux);
  return snapshot;
}

bool audio_service_get_network_state(AudioNetworkStateSnapshot* out_snapshot)
{
  if (!out_snapshot) return false;

  portENTER_CRITICAL(&s_state_mux);
  *out_snapshot = s_network_state_snapshot;
  portEXIT_CRITICAL(&s_state_mux);
  return true;
}

static void audio_task_service_playback_slice() {
  if (s_task_fade_gain > 0.0f) {
    audio_loop();
    s_task_playing = audio_is_playing();
  }
}

static void audio_task_wait_with_playback(uint32_t wait_ms)
{
  const uint32_t start_ms = millis();
  while ((millis() - start_ms) < wait_ms) {
    audio_task_service_playback_slice();
    vTaskDelay(1);
  }
}

static bool audio_task_soft_stop_impl(bool fast_fade)
{
  bool had_audio = audio_is_playing() || s_task_playing || (s_task_fade_gain > 0.0f);
  const uint32_t t0 = millis();
  uint32_t fade_iters = 0;

  if (had_audio) {
    const float step = fast_fade ? STOP_FADE_STEP : PAUSE_FADE_STEP;
    while (s_task_fade_gain > 0.0f && fade_iters < STOP_FADE_MAX_ITERS) {
      s_task_fade_gain -= step;
      if (s_task_fade_gain < 0.0f) s_task_fade_gain = 0.0f;
      audio_task_service_playback_slice();
      ++fade_iters;
      // 给 I2S DMA / decoder 一点推进时间，减少切歌瞬态破音
      vTaskDelay(1);
    }
  }

  audio_stop();
  s_task_playing = false;
  s_task_paused = false;
  // 保持静音起步，让下一首自然淡入，避免首包 PCM 直接“拍”到喇叭上
  s_task_fade_gain = 0.0f;
  s_task_last_fade_gain = 0.0f;
  audio_i2s_zero_dma_buffer();
  // 给零填充一个很短的生效窗口，进一步减少切歌 click/pop
  vTaskDelay(1);

  LOGD("[音频] 软停止：有音频=%d 淡出次数=%u 耗时=%lums",
       had_audio ? 1 : 0,
       (unsigned)fade_iters,
       (unsigned long)(millis() - t0));
  return had_audio;
}

static bool audio_task_fetch_total_ms_impl(const char* path, uint32_t* out_total_ms) {
  if (!path || !out_total_ms) return false;
  *out_total_ms = 0;
  const uint32_t t0 = millis();
  const uint32_t total_ms = audio_probe_total_ms(path);
  *out_total_ms = total_ms;
  LOGD("[音频] 总时长探测细节：耗时=%lums 总时长=%u",
       (unsigned long)(millis() - t0),
       (unsigned)total_ms);
  return total_ms > 0;
}

static bool audio_task_fetch_lyrics_impl(const char* path, char** out_text, size_t* out_len) {
  if (!path || !out_text || !out_len) return false;
  *out_text = nullptr;
  *out_len = 0;

  const uint32_t t0 = millis();
  uint32_t t_after_open = t0;
  uint32_t t_after_size = t0;
  uint32_t t_after_read = t0;
  uint32_t t_after_close = t0;

  AudioFile file;
  if (!file.open(sd, path)) {
    LOGW("[音频] 歌词 打开失败：%s", path ? path : "<null>");
    return false;
  }
  t_after_open = millis();

  uint32_t file_size = file.size();
  t_after_size = millis();

  if (file_size == 0 || file_size > 65536) {
    LOGW("[音频] 歌词 无效 大小=%u 路径=%s", (unsigned)file_size, path);
    file.close();
    return false;
  }

  char* buf = static_cast<char*>(alloc_aux_buffer((size_t)file_size + 1, "歌词缓冲"));
  if (!buf) {
    file.close();
    return false;
  }

  size_t copied = 0;
  bool ok = true;
  while (copied < file_size) {
    const size_t chunk = (file_size - copied > AUDIO_AUX_READ_CHUNK) ? AUDIO_AUX_READ_CHUNK : (file_size - copied);
    int r = file.read((uint8_t*)buf + copied, chunk);
    if (r != (int)chunk) {
      ok = false;
      break;
    }
    copied += chunk;
    audio_task_service_playback_slice();
  }
  t_after_read = millis();

  file.close();
  t_after_close = millis();

  if (!ok) {
    free(buf);
    return false;
  }

  buf[file_size] = '\0';
  *out_text = buf;
  *out_len = file_size;

  LOGD("[歌词][音频] 读取成功 路径=%s 大小=%u 指针=%p",
       path, (unsigned)file_size, buf);
  LOGD("[歌词][音频] 分配=%s 大小=%u 指针=%p",
       heap_caps_malloc_extmem_enable ? "psram_or_heap" : "heap",
       (unsigned)file_size, buf);

  const auto& st = file.last_open_stats();
  const uint32_t total_ms = t_after_close - t0;
  if (total_ms >= 20) {
    LOGD("[音频] 歌词 读取细节 等待锁=%lums 目录准备=%lums 目录缓存=%u 缓存原因=%s 打开=%lums 大小=%lums 读取=%lums 关闭=%lums 总计=%lums 字节=%u",
         (unsigned long)st.lock_wait_ms,
         (unsigned long)st.dir_prepare_ms,
         (unsigned)st.used_dir_cache,
         audio_file_dir_cache_reason_str(st.dir_cache_reason),
         (unsigned long)(t_after_open - t0),
         (unsigned long)(t_after_size - t_after_open),
         (unsigned long)(t_after_read - t_after_size),
         (unsigned long)(t_after_close - t_after_read),
         (unsigned long)total_ms,
         (unsigned)file_size);
  }
  return true;
}

static bool audio_task_fetch_cover_impl(AudioRequest& request) {
  request.result_buf = nullptr;
  request.result_buf_len = 0;
  request.result_is_png = false;

  const uint32_t t0 = millis();
  uint32_t t_after_open = t0;
  uint32_t t_after_size = t0;
  uint32_t t_after_seek = t0;
  uint32_t t_after_read = t0;
  uint32_t t_after_close = t0;

  String path;
  uint32_t size = 0;
  uint32_t offset = 0;

  if (request.cover_source == COVER_FILE_FALLBACK && request.cover_path[0]) {
    path = request.cover_path;
    offset = 0;
  } else if ((request.cover_source == COVER_MP3_APIC || request.cover_source == COVER_FLAC_PICTURE) &&
             request.cover_size > 0 && request.path[0]) {
    path = request.path;
    size = request.cover_size;
    offset = request.cover_offset;
  } else {
    return false;
  }

  AudioFile file;
  if (!file.open(sd, path.c_str())) {
    LOGW("[音频] 封面 打开失败：%s", path.c_str());
    return false;
  }
  t_after_open = millis();

  if (size == 0) {
    size = file.size();
  }
  t_after_size = millis();

  if (size == 0 || size > 400 * 1024u) {
    file.close();
    return false;
  }

  uint8_t* buf = static_cast<uint8_t*>(alloc_aux_buffer(size, "封面缓冲"));
  if (!buf) {
    file.close();
    return false;
  }

  if (!file.seek(offset)) {
    free(buf);
    file.close();
    return false;
  }
  t_after_seek = millis();

  bool ok = true;
  size_t copied = 0;
  while (copied < size) {
    const size_t chunk = (size - copied > AUDIO_AUX_READ_CHUNK) ? AUDIO_AUX_READ_CHUNK : (size - copied);
    int r = file.read(buf + copied, chunk);
    if (r != (int)chunk) {
      ok = false;
      break;
    }
    copied += chunk;
    audio_task_service_playback_slice();
  }
  t_after_read = millis();

  file.close();
  t_after_close = millis();

  if (!ok) {
    free(buf);
    return false;
  }

  bool is_png = detect_png_from_buffer(buf, size);
  if (!is_png && !detect_jpg_from_buffer(buf, size)) {
    LOGW("[音频] 封面 未知 头, 回退 JPEG 路径");
  }

  request.result_buf = buf;
  request.result_buf_len = size;
  request.result_is_png = is_png;

  const auto& st = file.last_open_stats();
  const uint32_t total_ms = t_after_close - t0;
  if (total_ms >= 20) {
    LOGD("[音频] 封面 读取细节 等待锁=%lums 目录准备=%lums 目录缓存=%u 缓存原因=%s 打开=%lums 大小=%lums 定位=%lums 读取=%lums 关闭=%lums 总计=%lums 字节=%u 来源=%u",
         (unsigned long)st.lock_wait_ms,
         (unsigned long)st.dir_prepare_ms,
         (unsigned)st.used_dir_cache,
         audio_file_dir_cache_reason_str(st.dir_cache_reason),
         (unsigned long)(t_after_open - t0),
         (unsigned long)(t_after_size - t_after_open),
         (unsigned long)(t_after_seek - t_after_size),
         (unsigned long)(t_after_read - t_after_seek),
         (unsigned long)(t_after_close - t_after_read),
         (unsigned long)total_ms,
         (unsigned)size,
         (unsigned)request.cover_source);
  }
  return true;
}

static void audio_task_keep_amp_safe_muted()
{
  (void)audio_output_route_set_amp_mute_from_audio_task(true);
}

static void audio_task_unmute_amp_if_route_allows()
{
  // 出声前重新读取当前路线。切歌/HTTP/预填充期间用户可能已切换输出。
  if (audio_output_route_is_speaker()) {
    (void)audio_output_route_set_amp_mute_from_audio_task(false);
  } else {
    (void)audio_output_route_set_amp_mute_from_audio_task(true);
  }
}

static void audio_task_prepare_amp_after_i2s_ready()
{
  // I2S 已初始化后，先把功放放到安全状态。
  // 非功放路线下不释放 SHDN，避免“仅耳机/蓝牙”模式被音频任务重新打开功放。
  audio_task_keep_amp_safe_muted();
  audio_i2s_zero_dma_buffer();

  vTaskDelay(pdMS_TO_TICKS(20));

  if (audio_output_route_is_speaker()) {
    (void)audio_output_route_set_amp_shutdown_from_audio_task(false);

    // 给 PAM8406 / 输出电容 / 模拟链路一点稳定时间。
    vTaskDelay(pdMS_TO_TICKS(150));

    // 注意：这里不要取消静音。
    // 真正开始播放且首批 PCM 推进后，再取消静音。
    audio_task_keep_amp_safe_muted();
    LOGD("[音频] 功放已准备：解除关断并保持静音");
  } else {
    (void)audio_output_route_set_amp_shutdown_from_audio_task(true);
    LOGD("[音频] 功放保持关闭：路线=%s", audio_output_route_label());
  }
}

static void audio_task_entry(void*){
  // I2S/decoder 初始化放在音频任务内部，确保由同一线程管理
  if (!audio_init()) {
    LOGE("[音频] init 失败 (AudioTask)");
  } else {
    audio_task_prepare_amp_after_i2s_ready();
  }

  s_task_ready = true;
  audio_task_publish_state();

  for (;;) {
    // 1) 先处理队列里的控制命令（暂停时依然可以接收停止命令）
    AudioRequest* request = nullptr;
    while (s_q && xQueueReceive(s_q, &request, 0) == pdTRUE) {
      if (!request) {
        LOGW("[音频] 收到空请求指针");
        continue;
      }

      AudioRequest& cmd = *request;
      bool success = true;

      if (cmd.type == CMD_STOP) {
        const uint32_t t_cmd = millis();
        audio_task_set_network_start_state(0, AudioNetworkStartPhase::Idle);

        audio_task_soft_stop_impl(true);

        // 停止后只静音，不关断功放。
        // 关断功放留到整机关机时再做，避免下一次播放 SHDN 跳变产生 pop。
        audio_task_keep_amp_safe_muted();

        const uint32_t t_done = millis();
        LOGD("[音频] 服务命令“停止”耗时=%lums 请求=%lu",
             (unsigned long)(t_done - t_cmd),
             (unsigned long)cmd.request_id);
      } else if (cmd.type == CMD_PLAY || cmd.type == CMD_PLAY_STREAM_MP3) {
        const uint32_t t_cmd = millis();

        // 播放前先保持功放静音，避免切歌/开机瞬态打到喇叭。
        audio_task_keep_amp_safe_muted();

        if (audio_is_playing() || s_task_playing || s_task_fade_gain > 0.0f) {
          audio_task_soft_stop_impl(true);
        } else {
          // 当前没在播时，仍清一下 DMA，但不要让新歌长期从极小音量起步
          s_task_fade_gain = 0.0f;
          s_task_last_fade_gain = 0.0f;
          s_task_paused = false;
          audio_i2s_zero_dma_buffer();
        }

        audio_task_keep_amp_safe_muted();

        bool ok = false;
        if (cmd.type == CMD_PLAY_STREAM_MP3) {
          if (!audio_mp3_audiotools_source_operation_is_current(cmd.network_operation_id)) {
            audio_task_set_network_start_state(cmd.request_id,
                                               AudioNetworkStartPhase::Cancelled,
                                               "cancelled");
          } else {
            audio_task_set_network_start_state(cmd.request_id, AudioNetworkStartPhase::Connecting);
            audio_task_publish_state();

            uint32_t start_offset = cmd.stream_start_offset;
            if (cmd.stream_probe_audio_offset) {
              uint32_t probed_offset = 0;
              if (audio_mp3_audiotools_source_probe_audio_start_offset(cmd.path,
                                                                         cmd.network_operation_id,
                                                                         &probed_offset)) {
                start_offset = probed_offset;
              }
            }

            if (audio_mp3_audiotools_source_operation_is_current(cmd.network_operation_id)) {
              if (start_offset > 0) {
                ok = audio_play_stream_mp3_from_offset(cmd.path,
                                                       start_offset,
                                                       cmd.network_operation_id);
                if (!ok &&
                    audio_mp3_audiotools_source_operation_is_current(cmd.network_operation_id)) {
                  LOGW("[音频] Range 起播失败，回退普通 URL 起播 offset=%lu",
                       (unsigned long)start_offset);
                  ok = audio_play_stream_mp3(cmd.path, cmd.network_operation_id);
                }
              } else {
                ok = audio_play_stream_mp3(cmd.path, cmd.network_operation_id);
              }
            }

            if (ok) {
              audio_task_set_network_start_state(cmd.request_id, AudioNetworkStartPhase::Playing);
            } else if (!audio_mp3_audiotools_source_operation_is_current(cmd.network_operation_id)) {
              audio_task_set_network_start_state(cmd.request_id,
                                                 AudioNetworkStartPhase::Cancelled,
                                                 "cancelled");
            } else if (!WiFi.isConnected()) {
              audio_task_set_network_start_state(cmd.request_id,
                                                 AudioNetworkStartPhase::Failed,
                                                 "wifi_disconnected");
            } else {
              audio_task_set_network_start_state(cmd.request_id,
                                                 AudioNetworkStartPhase::Failed,
                                                 "stream_start_failed");
            }
          }
        } else {
          audio_task_set_network_start_state(0, AudioNetworkStartPhase::Idle);
          ok = audio_play(cmd.path);
        }
        const uint32_t t_done = millis();
        success = ok;

        if (ok) {
          // 先用很小增益推进几帧 PCM，但功放仍静音。
          // 这样可以避开解码器刚打开、I2S 首包数据不稳定的瞬间。
          s_task_fade_gain = 0.05f;
          s_task_last_fade_gain = 0.05f;

          const uint32_t prefill_t0 = millis();
          const uint32_t primed_ms = audio_prime_pcm_ms(PLAY_PREFILL_TARGET_MS, PLAY_PREFILL_MAX_LOOPS);
          LOGD("[音频] 启动 software prefill primed_ms=%lu cost=%lums play_ms=%lu",
               (unsigned long)primed_ms,
               (unsigned long)(millis() - prefill_t0),
               (unsigned long)audio_i2s_get_play_ms());

          // 再恢复正常起播增益并取消静音。
          s_task_fade_gain = PLAY_START_GAIN;
          s_task_last_fade_gain = PLAY_START_GAIN;

          audio_task_unmute_amp_if_route_allows();
        } else {
          audio_task_keep_amp_safe_muted();
          s_task_fade_gain = 0.0f;
          s_task_last_fade_gain = 0.0f;
        }

        LOGD("[音频] 服务命令 %s 耗时=%lums 成功=%d 请求=%lu",
             (cmd.type == CMD_PLAY_STREAM_MP3) ? "play_stream_mp3" : "play",
             (unsigned long)(t_done - t_cmd),
             ok ? 1 : 0,
             (unsigned long)cmd.request_id);

        s_task_paused = false;
      } else if (cmd.type == CMD_PAUSE) {
        // 暂停请求只改变 AudioTask 内部状态；淡出和最终静音在主循环中完成。
        if (audio_is_playing() || s_task_playing || s_task_paused) {
          s_task_paused = true;
          success = true;
          LOGD("[音频] 服务命令“暂停”：请求=%lu", (unsigned long)cmd.request_id);
        } else {
          s_task_paused = false;
          s_task_fade_gain = 0.0f;
          s_task_last_fade_gain = 0.0f;
          audio_task_keep_amp_safe_muted();
          success = false;
          LOGD("[音频] 暂停请求已忽略：当前没有活动音频 请求=%lu",
               (unsigned long)cmd.request_id);
        }
      } else if (cmd.type == CMD_RESUME) {
        if (audio_is_playing() || s_task_playing) {
          // 先按当前路由决定是否打开功放，再由淡入状态机逐步恢复音量。
          audio_task_unmute_amp_if_route_allows();
          s_task_paused = false;
          success = true;
          LOGD("[音频] 服务命令“恢复”：请求=%lu", (unsigned long)cmd.request_id);
        } else {
          s_task_paused = false;
          audio_task_keep_amp_safe_muted();
          success = false;
          LOGD("[音频] 恢复请求已忽略：当前没有活动音频 请求=%lu",
               (unsigned long)cmd.request_id);
        }
      } else if (cmd.type == CMD_SET_OUTPUT_ROUTE) {
        // 路由切换期间先静音，避免功放、蓝牙和模拟输入切换产生瞬态噪声。
        audio_task_keep_amp_safe_muted();
        success = audio_output_route_apply_from_audio_task(cmd.output_route);

        if (success && audio_output_route_is_speaker()) {
          // 切回功放时等待模拟链路稳定，再根据当前播放状态决定是否取消静音。
          audio_task_wait_with_playback(150);
          if ((audio_is_playing() || s_task_playing) && !s_task_paused && s_task_fade_gain > 0.0f) {
            audio_task_unmute_amp_if_route_allows();
          } else {
            audio_task_keep_amp_safe_muted();
          }
        }

        LOGD("[音频] 服务命令“切换输出”：路线=%s 成功=%d 请求=%lu",
             audio_output_route_label(),
             success ? 1 : 0,
             (unsigned long)cmd.request_id);
      } else if (cmd.type == CMD_SET_AMP_MUTE) {
        success = audio_output_route_set_amp_mute_from_audio_task(cmd.enabled);
        LOGD("[音频] 服务命令“功放静音”：启用=%d 成功=%d 请求=%lu",
             cmd.enabled ? 1 : 0,
             success ? 1 : 0,
             (unsigned long)cmd.request_id);
      } else if (cmd.type == CMD_SET_AMP_SHUTDOWN) {
        // 关断前必须先静音；解除关断后保持静音，取消静音由播放/恢复流程决定。
        if (cmd.enabled) {
          audio_task_keep_amp_safe_muted();
        }
        success = audio_output_route_set_amp_shutdown_from_audio_task(cmd.enabled);
        if (!cmd.enabled) {
          audio_task_keep_amp_safe_muted();
        }
        LOGD("[音频] 服务命令“功放关断”：启用=%d 成功=%d 请求=%lu",
             cmd.enabled ? 1 : 0,
             success ? 1 : 0,
             (unsigned long)cmd.request_id);
      } else if (cmd.type == CMD_SET_USER_VOLUME) {
        success = audio_output_route_set_user_volume_from_audio_task(cmd.volume);
        LOGD("[音频] 服务命令“设置音量”：音量=%u 成功=%d 请求=%lu",
             (unsigned)cmd.volume,
             success ? 1 : 0,
             (unsigned long)cmd.request_id);
      } else if (cmd.type == CMD_FETCH_TOTAL_MS) {
        const uint32_t t_cmd = millis();
        const bool ok = audio_task_fetch_total_ms_impl(cmd.path, &cmd.result_total_ms);
        const uint32_t t_done = millis();
        success = ok;
        LOGD("[音频] 服务命令“读取总时长”：耗时=%lums 成功=%d 总时长=%u 请求=%lu",
             (unsigned long)(t_done - t_cmd),
             ok ? 1 : 0,
             (unsigned)cmd.result_total_ms,
             (unsigned long)cmd.request_id);
      } else if (cmd.type == CMD_FETCH_LYRICS) {
        const uint32_t t_cmd = millis();
        const bool ok = audio_task_fetch_lyrics_impl(cmd.path, &cmd.result_text, &cmd.result_text_len);
        const uint32_t t_done = millis();
        success = ok;
        LOGD("[音频] 服务命令“读取歌词”：耗时=%lums 成功=%d 字节=%u 请求=%lu",
             (unsigned long)(t_done - t_cmd),
             ok ? 1 : 0,
             (unsigned)(ok ? cmd.result_text_len : 0),
             (unsigned long)cmd.request_id);
      } else if (cmd.type == CMD_FETCH_COVER) {
        const uint32_t t_cmd = millis();
        const bool ok = audio_task_fetch_cover_impl(cmd);
        const uint32_t t_done = millis();
        success = ok;
        LOGD("[音频] 服务命令“读取封面”：耗时=%lums 成功=%d 字节=%u PNG=%d 来源=%u 请求=%lu",
             (unsigned long)(t_done - t_cmd),
             ok ? 1 : 0,
             (unsigned)(ok ? cmd.result_buf_len : 0),
             (int)(ok ? cmd.result_is_png : false),
             (unsigned)cmd.cover_source,
             (unsigned long)cmd.request_id);
      } else {
        success = false;
        LOGW("[音频] 未知请求类型=%u 请求=%lu",
             (unsigned)cmd.type,
             (unsigned long)cmd.request_id);
      }

      cmd.success = success;

      // AudioTask 是运行状态的唯一写入者；命令完成前发布一致快照。
      s_task_playing = audio_is_playing();
      audio_task_publish_state();

      if (cmd.done) {
        if (xSemaphoreGive(cmd.done) != pdTRUE) {
          LOGW("[音频] 请求完成信号发送失败：请求=%lu", (unsigned long)cmd.request_id);
        }
      }

      // 释放 AudioTask 的队列引用。调用方超时后，请求会在这里安全销毁。
      audio_request_release(request);
      request = nullptr;
    }

    // --- 核心：淡入淡出状态机 ---
    if (s_task_paused) {
      if (s_task_fade_gain > 0.0f) {
        s_task_fade_gain -= PAUSE_FADE_STEP;
        if (s_task_fade_gain < 0.0f) s_task_fade_gain = 0.0f;
      }
    } else {
      if (s_task_fade_gain < 1.0f) {
        s_task_fade_gain += PLAY_FADE_STEP;
        if (s_task_fade_gain > 1.0f) s_task_fade_gain = 1.0f;
      }
    }

    // 检测淡出完成：清空 DMA 并由 AudioTask 静音功放，消除暂停后的底噪。
    if (s_task_last_fade_gain > 0.0f && s_task_fade_gain == 0.0f) {
      audio_i2s_zero_dma_buffer();
      if (s_task_paused) {
        audio_task_keep_amp_safe_muted();
      }
    }
    s_task_last_fade_gain = s_task_fade_gain;

    // 只有当增益大于 0，或者虽然暂停但还没淡出完成时，才跑循环
    if (s_task_fade_gain > 0.0f) {
      audio_loop();
    }

    // 2) 同步缓存：处理"自然播放结束"这种情况
    bool was_playing = s_task_playing;
    s_task_playing = audio_is_playing();

    // 播放结束自动复位：如果一首歌自然播放完了（EOF），自动复位暂停状态
    // 否则下一首歌可能会卡在暂停状态
    if (was_playing && !s_task_playing) {
      s_task_paused = false;
      s_task_fade_gain = 1.0f;
      s_task_last_fade_gain = 1.0f;
      audio_task_keep_amp_safe_muted();
    }

    audio_task_publish_state();

    // 3) 优化延迟：淡入淡出期间使用较短延迟
    if (s_task_fade_gain > 0.0f && s_task_fade_gain < 1.0f) {
      vTaskDelay(1);
    } else if (!s_task_playing || s_task_paused) {
      vTaskDelay(10);
    } else {
      // 网络 MP3 的 WiFiClient::available()/read 可能被 AudioTask 高频轮询，
      // 如果这里只 taskYIELD，低优先级 IDLE0 仍然可能长期得不到运行，触发 task_wdt。
      // 仅对网络 MP3 每轮让出 1 tick；本地 FLAC/MP3 仍保持原来的轻量 yield，
      // 避免本地高码率解码被额外延时拖慢。
      if (audio_mp3_is_active() && audio_mp3_is_stream_source()) {
        vTaskDelay(1);
      } else {
        taskYIELD();
      }
    }
  }
}

void audio_service_start(void)
{
  if (s_task) return;

  s_q = xQueueCreate(AUDIO_CMD_QUEUE_LEN, sizeof(AudioRequest*));
  if (!s_q) {
    LOGE("[音频] 创建命令队列失败：长度=%u", (unsigned)AUDIO_CMD_QUEUE_LEN);
    return;
  }

  LOGD("[音频] 命令队列已创建：长度=%u 元素大小=%u 字节",
       (unsigned)AUDIO_CMD_QUEUE_LEN,
       (unsigned)sizeof(AudioRequest*));

  const BaseType_t created = xTaskCreatePinnedToCore(audio_task_entry,
                                                     "AudioTask",
                                                     kAudioTaskStackBytes,
                                                     nullptr,
                                                     AUDIO_TASK_PRIO,
                                                     &s_task,
                                                     AUDIO_TASK_CORE);
  if (created != pdPASS || !s_task) {
    LOGE("[音频] 创建 AudioTask 失败：返回值=%ld", (long)created);
    s_task = nullptr;
    vQueueDelete(s_q);
    s_q = nullptr;
  }
}

static bool wait_ready(uint32_t timeout_ms)
{
  const uint32_t t0 = millis();
  while (!audio_service_read_state_snapshot().ready && (millis() - t0) < timeout_ms) {
    vTaskDelay(1);
  }
  return audio_service_read_state_snapshot().ready;
}

static uint32_t audio_request_timeout_ms(AudioCmdType type)
{
  switch (type) {
    case CMD_STOP:
      return 3000;
    case CMD_PLAY:
      return 5000;
    case CMD_PLAY_STREAM_MP3:
      return 12000;
    case CMD_FETCH_LYRICS:
      return 3000;
    case CMD_FETCH_COVER:
    case CMD_FETCH_TOTAL_MS:
      return 5000;
    case CMD_PAUSE:
    case CMD_RESUME:
    case CMD_SET_AMP_MUTE:
    case CMD_SET_AMP_SHUTDOWN:
    case CMD_SET_USER_VOLUME:
      return 2000;
    case CMD_SET_OUTPUT_ROUTE:
      return 4000;
    default:
      return 2000;
  }
}

static bool dispatch_request(AudioRequest* request, bool wait)
{
  if (!request) return false;

  if (!wait_ready(1000)) {
    LOGW("[音频] AudioTask 未就绪：请求=%lu 类型=%u",
         (unsigned long)request->request_id,
         (unsigned)request->type);
    return false;
  }
  if (!s_q) {
    LOGW("[音频] 命令队列不可用：请求=%lu", (unsigned long)request->request_id);
    return false;
  }
  if (wait && !request->done) {
    LOGE("[音频] 同步请求缺少完成信号量：请求=%lu", (unsigned long)request->request_id);
    return false;
  }

  // 入队前增加 AudioTask 引用。即使调用方随后等待超时，请求对象仍然有效。
  audio_request_retain(request);
  AudioRequest* queued_request = request;

  // 使用 100ms 超时，避免 UI 线程因队列满而长时间阻塞。
  if (xQueueSend(s_q, &queued_request, pdMS_TO_TICKS(100)) != pdTRUE) {
    LOGW("[音频] 发送请求超时，音频核心忙：请求=%lu 类型=%u",
         (unsigned long)request->request_id,
         (unsigned)request->type);
    audio_request_release(request); // 归还尚未进入队列的 AudioTask 引用
    return false;
  }

  if (!wait) {
    return true;
  }

  const uint32_t timeout_ms = audio_request_timeout_ms(request->type);
  if (xSemaphoreTake(request->done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
    LOGW("[音频] 等待请求完成超时：请求=%lu 类型=%u 超时=%lums；请求将在 AudioTask 完成后释放",
         (unsigned long)request->request_id,
         (unsigned)request->type,
         (unsigned long)timeout_ms);
    return false;
  }

  return request->success;
}

bool audio_service_play(const char* path, bool wait)
{
  if (!path) return false;

  // 本地播放会取代正在连接或播放的网络流。这里只更新取消编号，不直接访问 WiFiClient。
  audio_mp3_audiotools_source_cancel_operation();

  AudioRequest* request = audio_request_new(wait);
  if (!request) return false;
  request->type = CMD_PLAY;
  strncpy(request->path, path, sizeof(request->path) - 1);
  request->path[sizeof(request->path) - 1] = '\0';

  const bool ok = dispatch_request(request, wait);
  audio_request_release(request); // 释放调用方引用
  return ok;
}

static bool audio_service_queue_stream_request(const char* url,
                                               uint32_t start_offset,
                                               bool probe_audio_offset,
                                               bool wait,
                                               uint32_t* out_request_id)
{
  if (out_request_id) *out_request_id = 0;
  if (!url || !*url) return false;

  AudioRequest* request = audio_request_new(wait);
  if (!request) return false;

  // 新操作编号会使上一条网络连接/读取路径尽快退出。
  const uint32_t operation_id = audio_mp3_audiotools_source_begin_operation();
  request->type = CMD_PLAY_STREAM_MP3;
  request->network_operation_id = operation_id;
  request->stream_start_offset = start_offset;
  request->stream_probe_audio_offset = probe_audio_offset;
  strncpy(request->path, url, sizeof(request->path) - 1);
  request->path[sizeof(request->path) - 1] = '\0';

  const uint32_t request_id = request->request_id;
  const bool ok = dispatch_request(request, wait);
  if (ok && out_request_id) {
    *out_request_id = request_id;
  }
  if (!ok && audio_mp3_audiotools_source_operation_is_current(operation_id)) {
    audio_mp3_audiotools_source_cancel_operation();
  }
  audio_request_release(request);
  return ok;
}

bool audio_service_play_stream_mp3(const char* url, bool wait)
{
  return audio_service_queue_stream_request(url, 0, false, wait, nullptr);
}

bool audio_service_play_stream_mp3_from_offset(const char* url, uint32_t start_offset, bool wait)
{
  return audio_service_queue_stream_request(url, start_offset, false, wait, nullptr);
}

bool audio_service_play_stream_mp3_async(const char* url, uint32_t* out_request_id)
{
  return audio_service_queue_stream_request(url, 0, false, false, out_request_id);
}

bool audio_service_play_stream_mp3_auto_offset_async(const char* url, uint32_t* out_request_id)
{
  return audio_service_queue_stream_request(url, 0, true, false, out_request_id);
}

bool audio_service_stop(bool wait)
{
  // 先使正在连接/预缓冲的网络操作失效，AudioTask 会在短连接切片或读取循环中退出。
  audio_mp3_audiotools_source_cancel_operation();

  AudioRequest* request = audio_request_new(wait);
  if (!request) return false;
  request->type = CMD_STOP;

  const bool ok = dispatch_request(request, wait);
  audio_request_release(request);
  return ok;
}

bool audio_service_is_playing(void)
{
  return audio_service_read_state_snapshot().playing;
}

bool audio_service_fetch_total_ms(const char* path, uint32_t* out_total_ms, bool wait)
{
  if (!path || !out_total_ms) return false;
  *out_total_ms = 0;

  // 读取类请求必须同步等待，否则调用方无法安全接管返回结果。
  if (!wait) {
    LOGW("[音频] 读取总时长不支持 wait=false：%s", path);
    return false;
  }

  AudioRequest* request = audio_request_new(true);
  if (!request) return false;
  request->type = CMD_FETCH_TOTAL_MS;
  strncpy(request->path, path, sizeof(request->path) - 1);
  request->path[sizeof(request->path) - 1] = '\0';

  const bool ok = dispatch_request(request, true);
  if (ok) {
    *out_total_ms = request->result_total_ms;
  }
  audio_request_release(request);
  return ok;
}

bool audio_service_fetch_lyrics(const char* path, char** out_text, size_t* out_len, bool wait)
{
  if (!path || !out_text || !out_len) return false;
  *out_text = nullptr;
  *out_len = 0;

  if (!wait) {
    LOGW("[音频] 读取歌词不支持 wait=false：%s", path);
    return false;
  }

  AudioRequest* request = audio_request_new(true);
  if (!request) return false;
  request->type = CMD_FETCH_LYRICS;
  strncpy(request->path, path, sizeof(request->path) - 1);
  request->path[sizeof(request->path) - 1] = '\0';

  const bool ok = dispatch_request(request, true);
  if (ok) {
    *out_text = request->result_text;
    *out_len = request->result_text_len;
    request->result_text = nullptr; // 结果所有权转交给调用方
    request->result_text_len = 0;
  }
  audio_request_release(request);
  return ok;
}

bool audio_service_fetch_cover(CoverSource cover_source,
                               const char* audio_path,
                               const char* cover_path,
                               uint32_t cover_offset,
                               uint32_t cover_size,
                               uint8_t** out_buf,
                               size_t* out_len,
                               bool* out_is_png,
                               bool wait)
{
  if (!out_buf || !out_len || !out_is_png) return false;
  *out_buf = nullptr;
  *out_len = 0;
  *out_is_png = false;

  if (!wait) {
    LOGW("[音频] 读取封面不支持 wait=false");
    return false;
  }

  AudioRequest* request = audio_request_new(true);
  if (!request) return false;
  request->type = CMD_FETCH_COVER;
  request->cover_source = cover_source;
  request->cover_offset = cover_offset;
  request->cover_size = cover_size;
  if (audio_path) {
    strncpy(request->path, audio_path, sizeof(request->path) - 1);
    request->path[sizeof(request->path) - 1] = '\0';
  }
  if (cover_path) {
    strncpy(request->cover_path, cover_path, sizeof(request->cover_path) - 1);
    request->cover_path[sizeof(request->cover_path) - 1] = '\0';
  }

  const bool ok = dispatch_request(request, true);
  if (ok) {
    *out_buf = request->result_buf;
    *out_len = request->result_buf_len;
    *out_is_png = request->result_is_png;
    request->result_buf = nullptr; // 结果所有权转交给调用方
    request->result_buf_len = 0;
  }
  audio_request_release(request);
  return ok;
}

static bool audio_service_dispatch_control(AudioCmdType type,
                                           bool enabled,
                                           AudioOutputRoute route,
                                           uint8_t volume,
                                           bool wait)
{
  AudioRequest* request = audio_request_new(wait);
  if (!request) return false;

  request->type = type;
  request->enabled = enabled;
  request->output_route = route;
  request->volume = volume;

  const bool ok = dispatch_request(request, wait);
  audio_request_release(request);
  return ok;
}

bool audio_service_pause(bool wait)
{
  return audio_service_dispatch_control(CMD_PAUSE, false, AudioOutputRoute::Speaker, 0, wait);
}

bool audio_service_resume(bool wait)
{
  return audio_service_dispatch_control(CMD_RESUME, false, AudioOutputRoute::Speaker, 0, wait);
}

bool audio_service_set_output_route(AudioOutputRoute route, bool wait)
{
  if (route != AudioOutputRoute::HeadphoneOnly &&
      route != AudioOutputRoute::Speaker &&
      route != AudioOutputRoute::BluetoothTx) {
    LOGW("[音频] 拒绝无效输出路线=%u", (unsigned)route);
    return false;
  }
  return audio_service_dispatch_control(CMD_SET_OUTPUT_ROUTE, false, route, 0, wait);
}

bool audio_service_set_amp_mute(bool enabled, bool wait)
{
  return audio_service_dispatch_control(CMD_SET_AMP_MUTE, enabled, AudioOutputRoute::Speaker, 0, wait);
}

bool audio_service_set_amp_shutdown(bool enabled, bool wait)
{
  return audio_service_dispatch_control(CMD_SET_AMP_SHUTDOWN, enabled, AudioOutputRoute::Speaker, 0, wait);
}


bool audio_service_set_user_volume(uint8_t value, bool wait)
{
  if (value > 100) value = 100;
  return audio_service_dispatch_control(CMD_SET_USER_VOLUME, false, AudioOutputRoute::Speaker, value, wait);
}

bool audio_service_is_paused(void)
{
  return audio_service_read_state_snapshot().paused;
}

float audio_service_get_fade_gain(void)
{
  // 解码器在 AudioTask 内取增益时直接读取任务私有状态，避免快照延迟一帧。
  if (s_task && xTaskGetCurrentTaskHandle() == s_task) {
    return s_task_fade_gain;
  }
  return audio_service_read_state_snapshot().fade_gain;
}

TaskHandle_t audio_service_get_task_handle(void)
{
  return s_task;
}
