#include <Arduino.h>
#include <string.h>
#include <stdlib.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <esp32-hal-psram.h>
#include <SdFat.h>

#include "audio/audio_service.h"
#include "audio/audio.h"
#include "audio/audio_file.h"
#include "audio/audio_i2s.h"
#include "storage/storage_io.h"
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
#define AUDIO_CMD_PATH_MAX 256
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
  CMD_PLAY_STREAM_MP3 = 6,
  CMD_FETCH_LYRICS = 3,
  CMD_FETCH_COVER = 4,
  CMD_FETCH_TOTAL_MS = 5,
};

struct AudioCmd {
  AudioCmdType type;
  char path[AUDIO_CMD_PATH_MAX];       // play / lyrics / embedded cover audio path
  char cover_path[AUDIO_CMD_PATH_MAX]; // fallback cover file path
  TaskHandle_t notify_to;              // wait=true 时，用 notify ACK

  CoverSource cover_source = COVER_NONE;
  uint32_t cover_offset = 0;
  uint32_t cover_size = 0;

  char** out_text = nullptr;
  size_t* out_text_len = nullptr;
  uint32_t* out_total_ms = nullptr;

  uint8_t** out_buf = nullptr;
  size_t* out_buf_len = nullptr;
  bool* out_is_png = nullptr;
};

static QueueHandle_t s_q = nullptr;
static TaskHandle_t  s_task = nullptr;
static volatile bool s_playing_cache = false;
static volatile bool s_ready = false;
static bool s_paused = false; // 内部暂停标志

// 淡入淡出功能相关变量
static float s_fade_gain = 1.0f;      // 当前增益 (0.0 到 1.0)
static float s_last_fade_gain = 1.0f; // 上一次的增益

#define PAUSE_FADE_STEP   0.05f   // 暂停时淡出，保持柔和
#define PLAY_FADE_STEP    0.12f   // 开播/切歌时淡入，加快恢复正常音量
#define PLAY_START_GAIN   0.35f   // 新歌起播初始增益，别从 0 开始
#define STOP_FADE_MAX_ITERS 8
#define STOP_FADE_STEP    0.20f

static inline bool detect_png_from_buffer(const uint8_t* b, size_t len) {
  return (len >= 8 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G' &&
          b[4] == 0x0D && b[5] == 0x0A && b[6] == 0x1A && b[7] == 0x0A);
}

static inline bool detect_jpg_from_buffer(const uint8_t* b, size_t len) {
  return (len >= 2 && b[0] == 0xFF && b[1] == 0xD8);
}

static void audio_task_service_playback_slice() {
  if (s_fade_gain > 0.0f) {
    audio_loop();
    s_playing_cache = audio_is_playing();
  }
}

static bool audio_task_soft_stop_impl(bool fast_fade)
{
  bool had_audio = audio_is_playing() || s_playing_cache || (s_fade_gain > 0.0f);
  const uint32_t t0 = millis();
  uint32_t fade_iters = 0;

  if (had_audio) {
    const float step = fast_fade ? STOP_FADE_STEP : PAUSE_FADE_STEP;
    while (s_fade_gain > 0.0f && fade_iters < STOP_FADE_MAX_ITERS) {
      s_fade_gain -= step;
      if (s_fade_gain < 0.0f) s_fade_gain = 0.0f;
      audio_task_service_playback_slice();
      ++fade_iters;
      // 给 I2S DMA / decoder 一点推进时间，减少切歌瞬态破音
      vTaskDelay(1);
    }
  }

  audio_stop();
  s_playing_cache = false;
  s_paused = false;
  // 保持静音起步，让下一首自然淡入，避免首包 PCM 直接“拍”到喇叭上
  s_fade_gain = 0.0f;
  s_last_fade_gain = 0.0f;
  audio_i2s_zero_dma_buffer();
  // 给零填充一个很短的生效窗口，进一步减少切歌 click/pop
  vTaskDelay(1);

  LOGD("[AUDIO] soft stop had_audio=%d fade_iters=%u exec=%lums",
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
  LOGD("[AUDIO] total probe detail exec=%lums total_ms=%u",
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
    LOGW("[AUDIO] lyrics open failed: %s", path ? path : "<null>");
    return false;
  }
  t_after_open = millis();

  uint32_t file_size = file.size();
  t_after_size = millis();

  if (file_size == 0 || file_size > 65536) {
    LOGW("[AUDIO] lyrics invalid size=%u path=%s", (unsigned)file_size, path);
    file.close();
    return false;
  }

  char* buf = (char*)ps_malloc(file_size + 1);
  if (!buf) {
    buf = (char*)malloc(file_size + 1);
  }
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

  LOGI("[LYRICS][AUDIO] fetch ok path=%s size=%u ptr=%p",
       path, (unsigned)file_size, buf);
  LOGI("[LYRICS][AUDIO] alloc=%s size=%u ptr=%p",
       heap_caps_malloc_extmem_enable ? "psram_or_heap" : "heap",
       (unsigned)file_size, buf);

  const auto& st = file.last_open_stats();
  const uint32_t total_ms = t_after_close - t0;
  if (total_ms >= 20) {
    LOGD("[AUDIO] lyrics fetch detail lock_wait=%lums dir_prepare=%lums dir_cache=%u cache_reason=%s open=%lums size=%lums read=%lums close=%lums total=%lums bytes=%u",
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

static bool audio_task_fetch_cover_impl(const AudioCmd& cmd) {
  if (!cmd.out_buf || !cmd.out_buf_len || !cmd.out_is_png) return false;
  *cmd.out_buf = nullptr;
  *cmd.out_buf_len = 0;
  *cmd.out_is_png = false;

  const uint32_t t0 = millis();
  uint32_t t_after_open = t0;
  uint32_t t_after_size = t0;
  uint32_t t_after_seek = t0;
  uint32_t t_after_read = t0;
  uint32_t t_after_close = t0;

  String path;
  uint32_t size = 0;
  uint32_t offset = 0;

  if (cmd.cover_source == COVER_FILE_FALLBACK && cmd.cover_path[0]) {
    path = cmd.cover_path;
    offset = 0;
  } else if ((cmd.cover_source == COVER_MP3_APIC || cmd.cover_source == COVER_FLAC_PICTURE) &&
             cmd.cover_size > 0 && cmd.path[0]) {
    path = cmd.path;
    size = cmd.cover_size;
    offset = cmd.cover_offset;
  } else {
    return false;
  }

  AudioFile file;
  if (!file.open(sd, path.c_str())) {
    LOGW("[AUDIO] cover open failed: %s", path.c_str());
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

  uint8_t* buf = (uint8_t*)ps_malloc(size);
  if (!buf) buf = (uint8_t*)malloc(size);
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
    LOGW("[AUDIO] cover unknown header, fallback JPEG path");
  }

  *cmd.out_buf = buf;
  *cmd.out_buf_len = size;
  *cmd.out_is_png = is_png;

  const auto& st = file.last_open_stats();
  const uint32_t total_ms = t_after_close - t0;
  if (total_ms >= 20) {
    LOGD("[AUDIO] cover fetch detail lock_wait=%lums dir_prepare=%lums dir_cache=%u cache_reason=%s open=%lums size=%lums seek=%lums read=%lums close=%lums total=%lums bytes=%u src=%u",
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
         (unsigned)cmd.cover_source);
  }
  return true;
}

static void audio_task_entry(void*)
{
  // I2S/decoder 初始化放在音频任务内部，确保由同一线程管理
  if (!audio_init()) {
    Serial.println("[AUDIO] init failed (AudioTask)");
  }

  s_ready = true;

  for (;;) {
    // 1) 先处理队列里的控制命令（保持不变，这样暂停时依然可以发 CMD_STOP 停止播放）
    AudioCmd cmd;
    while (s_q && xQueueReceive(s_q, &cmd, 0) == pdTRUE) {
      uint32_t ack = 1;

      if (cmd.type == CMD_STOP) {
        const uint32_t t_cmd = millis();
        audio_task_soft_stop_impl(true);
        const uint32_t t_done = millis();
        LOGD("[AUDIO] service cmd stop exec=%lums", (unsigned long)(t_done - t_cmd));
      } else if (cmd.type == CMD_PLAY || cmd.type == CMD_PLAY_STREAM_MP3) {
        const uint32_t t_cmd = millis();
        if (audio_is_playing() || s_playing_cache || s_fade_gain > 0.0f) {
          audio_task_soft_stop_impl(true);
        } else {
          // 当前没在播时，仍清一下 DMA，但不要让新歌长期从极小音量起步
          s_fade_gain = 0.0f;
          s_last_fade_gain = 0.0f;
          s_paused = false;
          audio_i2s_zero_dma_buffer();
        }

        bool ok = (cmd.type == CMD_PLAY_STREAM_MP3) ? audio_play_stream_mp3(cmd.path) : audio_play(cmd.path);
        const uint32_t t_done = millis();
        ack = ok ? 1 : 0;

        if (ok) {
          // 切歌起播不要从 0 开始，避免一开始声音明显偏小
          s_fade_gain = PLAY_START_GAIN;
          s_last_fade_gain = PLAY_START_GAIN;
        } else {
          s_fade_gain = 0.0f;
          s_last_fade_gain = 0.0f;
        }

        LOGD("[AUDIO] service cmd %s exec=%lums ok=%d",
            (cmd.type == CMD_PLAY_STREAM_MP3) ? "play_stream_mp3" : "play",
            (unsigned long)(t_done - t_cmd), ok ? 1 : 0);

        s_paused = false;
      }
       else if (cmd.type == CMD_FETCH_TOTAL_MS) {
        const uint32_t t_cmd = millis();
        bool ok = audio_task_fetch_total_ms_impl(cmd.path, cmd.out_total_ms);
        const uint32_t t_done = millis();
        ack = ok ? 1 : 0;
        LOGD("[AUDIO] service cmd fetch_total exec=%lums ok=%d total_ms=%u",
             (unsigned long)(t_done - t_cmd), ok ? 1 : 0,
             (unsigned)((cmd.out_total_ms) ? *cmd.out_total_ms : 0));
      } else if (cmd.type == CMD_FETCH_LYRICS) {
        const uint32_t t_cmd = millis();
        bool ok = audio_task_fetch_lyrics_impl(cmd.path, cmd.out_text, cmd.out_text_len);
        const uint32_t t_done = millis();
        ack = ok ? 1 : 0;
        LOGD("[AUDIO] service cmd fetch_lyrics exec=%lums ok=%d bytes=%u",
             (unsigned long)(t_done - t_cmd), ok ? 1 : 0,
             (unsigned)((ok && cmd.out_text_len) ? *cmd.out_text_len : 0));
      } else if (cmd.type == CMD_FETCH_COVER) {
        const uint32_t t_cmd = millis();
        bool ok = audio_task_fetch_cover_impl(cmd);
        const uint32_t t_done = millis();
        ack = ok ? 1 : 0;
        LOGD("[AUDIO] service cmd fetch_cover exec=%lums ok=%d bytes=%u png=%d src=%u",
             (unsigned long)(t_done - t_cmd), ok ? 1 : 0,
             (unsigned)((ok && cmd.out_buf_len) ? *cmd.out_buf_len : 0),
             (int)((ok && cmd.out_is_png) ? *cmd.out_is_png : 0),
             (unsigned)cmd.cover_source);
      }

      // 刷新播放状态缓存
      s_playing_cache = audio_is_playing();

      if (cmd.notify_to) {
        xTaskNotify(cmd.notify_to, ack, eSetValueWithOverwrite);
      }
    }

    // --- 核心：淡入淡出状态机 ---
    if (s_paused) {
      if (s_fade_gain > 0.0f) {
        s_fade_gain -= PAUSE_FADE_STEP;
        if (s_fade_gain < 0.0f) s_fade_gain = 0.0f;
      }
    } else {
      if (s_fade_gain < 1.0f) {
        s_fade_gain += PLAY_FADE_STEP;
        if (s_fade_gain > 1.0f) s_fade_gain = 1.0f;
      }
    }

    // 检测淡出完成，清空 DMA 缓冲区消除底噪
    if (s_last_fade_gain > 0.0f && s_fade_gain == 0.0f) {
      audio_i2s_zero_dma_buffer();
    }
    s_last_fade_gain = s_fade_gain;

    // 只有当增益大于 0，或者虽然暂停但还没淡出完成时，才跑循环
    if (s_fade_gain > 0.0f) {
      audio_loop();
    }

    // 2) 同步缓存：处理"自然播放结束"这种情况
    bool was_playing = s_playing_cache;
    s_playing_cache = audio_is_playing();

    // 播放结束自动复位：如果一首歌自然播放完了（EOF），自动复位暂停状态
    // 否则下一首歌可能会卡在暂停状态
    if (was_playing && !s_playing_cache) {
      s_paused = false;
      s_fade_gain = 1.0f;
      s_last_fade_gain = 1.0f;
    }

    // 3) 优化延迟：淡入淡出期间使用较短延迟
    if (s_fade_gain > 0.0f && s_fade_gain < 1.0f) {
      vTaskDelay(1);
    } else if (!s_playing_cache || s_paused) {
      vTaskDelay(10);
    } else {
      vTaskDelay(1);
    }
  }
}

void audio_service_start(void)
{
  if (s_task) return;

  s_q = xQueueCreate(AUDIO_CMD_QUEUE_LEN, sizeof(AudioCmd));
  LOGI("[AUDIO] cmd queue len=%u cmd_size=%u bytes",
       (unsigned)AUDIO_CMD_QUEUE_LEN,
       (unsigned)sizeof(AudioCmd));
  xTaskCreatePinnedToCore(audio_task_entry,
                          "AudioTask",
                          kAudioTaskStackBytes,
                          nullptr,
                          AUDIO_TASK_PRIO,
                          &s_task,
                          AUDIO_TASK_CORE);
}

static bool wait_ready(uint32_t timeout_ms)
{
  uint32_t t0 = millis();
  while (!s_ready && (millis() - t0) < timeout_ms) {
    vTaskDelay(1);
  }
  return s_ready;
}

static bool send_cmd(AudioCmd& cmd, bool wait)
{
  if (!wait_ready(1000)) return false;
  if (!s_q) return false;

  cmd.notify_to = wait ? xTaskGetCurrentTaskHandle() : nullptr;

  // 使用 100ms 超时，避免 UI 线程卡死
  if (xQueueSend(s_q, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
    Serial.println("[AUDIO] 发送命令超时，音频核心忙");
    return false;
  }
  if (!wait) return true;

  uint32_t ack = 0;
  if (xTaskNotifyWait(0, 0xFFFFFFFF, &ack, pdMS_TO_TICKS(2000)) != pdTRUE) return false;
  return ack == 1;
}

bool audio_service_play(const char* path, bool wait)
{
  if (!path) return false;

  AudioCmd cmd{};
  cmd.type = CMD_PLAY;
  strncpy(cmd.path, path, sizeof(cmd.path) - 1);
  cmd.path[sizeof(cmd.path) - 1] = '\0';

  return send_cmd(cmd, wait);
}


bool audio_service_play_stream_mp3(const char* url, bool wait)
{
  if (!url) return false;
  AudioCmd cmd{};
  cmd.type = CMD_PLAY_STREAM_MP3;
  strncpy(cmd.path, url, sizeof(cmd.path) - 1);
  cmd.path[sizeof(cmd.path) - 1] = '\0';
  return send_cmd(cmd, wait);
}

bool audio_service_stop(bool wait)
{
  AudioCmd cmd{};
  cmd.type = CMD_STOP;
  cmd.path[0] = 0;
  return send_cmd(cmd, wait);
}

bool audio_service_is_playing(void)
{
  return s_playing_cache;
}

bool audio_service_fetch_total_ms(const char* path, uint32_t* out_total_ms, bool wait)
{
  if (!path || !out_total_ms) return false;
  AudioCmd cmd{};
  cmd.type = CMD_FETCH_TOTAL_MS;
  strncpy(cmd.path, path, sizeof(cmd.path) - 1);
  cmd.path[sizeof(cmd.path) - 1] = '\0';
  cmd.notify_to = wait ? xTaskGetCurrentTaskHandle() : nullptr;
  cmd.out_total_ms = out_total_ms;
  return send_cmd(cmd, wait);
}

bool audio_service_fetch_lyrics(const char* path, char** out_text, size_t* out_len, bool wait)
{
  if (!path || !out_text || !out_len) return false;
  *out_text = nullptr;
  *out_len = 0;

  AudioCmd cmd{};
  cmd.type = CMD_FETCH_LYRICS;
  strncpy(cmd.path, path, sizeof(cmd.path) - 1);
  cmd.path[sizeof(cmd.path) - 1] = '\0';
  cmd.out_text = out_text;
  cmd.out_text_len = out_len;
  return send_cmd(cmd, wait);
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

  AudioCmd cmd{};
  cmd.type = CMD_FETCH_COVER;
  cmd.cover_source = cover_source;
  cmd.cover_offset = cover_offset;
  cmd.cover_size = cover_size;
  if (audio_path) {
    strncpy(cmd.path, audio_path, sizeof(cmd.path) - 1);
    cmd.path[sizeof(cmd.path) - 1] = '\0';
  }
  if (cover_path) {
    strncpy(cmd.cover_path, cover_path, sizeof(cmd.cover_path) - 1);
    cmd.cover_path[sizeof(cmd.cover_path) - 1] = '\0';
  }
  cmd.out_buf = out_buf;
  cmd.out_buf_len = out_len;
  cmd.out_is_png = out_is_png;
  return send_cmd(cmd, wait);
}

// 供外部调用的暂停控制接口
void audio_service_pause() {
    s_paused = true;
    // 注意：不再立即调用 zero_dma_buffer，因为我们要留时间做淡出
}
void audio_service_resume() {
    s_paused = false;
}
bool audio_service_is_paused() { return s_paused; }

// 获取当前淡入淡出增益
float audio_service_get_fade_gain() { return s_fade_gain; }

TaskHandle_t audio_service_get_task_handle(void)
{
  return s_task;
}
