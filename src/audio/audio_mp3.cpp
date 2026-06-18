// Unified MP3 decode core
// 职责：
// 1) 维护统一的 MP3 解码后半段：inbuf / minimp3 decode / resync / PCM pending / I2S 输出
// 2) 不关心输入来自文件还是网络
// 3) 文件和网络输入都通过 AudioMp3Source 适配接入

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "audio/audio_mp3.h"
#include "audio/audio_i2s.h"
#include "audio/audio_file.h"
#include "audio/audio_mp3_source_file.h"
#include "audio/audio_mp3_source_audiotools.h"
#include "utils/log.h"

#define MINIMP3_IMPLEMENTATION
#include "../../lib/minimp3/minimp3.h"

namespace {
static mp3dec_t g_dec;
static AudioMp3Source g_source{};
static bool g_source_active = false;
static bool g_source_is_stream = false;
static bool g_source_eof = false;

// 主线状态变量
static bool s_mp3_active = false;
static uint32_t s_mp3_sample_rate = 0;
static uint8_t s_mp3_channels = 0;
static uint32_t s_mp3_bitrate_kbps = 0;
static String s_mp3_last_error;
static String s_mp3_debug_name;

static constexpr size_t kMp3FileInputBufferBytes = 8 * 1024;
// 网络 MP3 流比本地文件更怕 UI/菜单短时间抢 CPU。
// 这里给网络流单独使用更大的输入缓冲，并优先放到 PSRAM。
static constexpr size_t kMp3StreamInputBufferBytes = 32 * 1024;
static constexpr size_t kMp3StreamStartupPrefillBytes = 16 * 1024;
static constexpr size_t kMp3StreamRefillLowBytes = 12 * 1024;
static constexpr size_t kMp3StreamRefillTargetBytes = 28 * 1024;
static constexpr uint32_t kMp3StreamStartupPrefillTimeoutMs = 1500;

static uint8_t s_file_inbuf[kMp3FileInputBufferBytes];
static uint8_t* g_inbuf = s_file_inbuf;
static size_t g_inbuf_capacity = sizeof(s_file_inbuf);
static bool g_inbuf_is_psram = false;
static int g_inbuf_filled = 0;
static bool g_playing = false;
static int g_sr = 44100;

static int16_t g_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME * 2]; // stereo interleaved (预留双声道空间)
static size_t s_pending_off = 0;
static size_t s_pending_frames = 0;
static int s_channels = 2; // 当前声道数
static int s_last_sr = 0; // 上次设置的采样率（文件级 static，便于重置）
static const char* s_debug_name = nullptr;

static void release_stream_input_buffer()
{
  if (g_inbuf && g_inbuf != s_file_inbuf) {
    free(g_inbuf);
  }
  g_inbuf = s_file_inbuf;
  g_inbuf_capacity = sizeof(s_file_inbuf);
  g_inbuf_is_psram = false;
}

static bool select_input_buffer_for_source(bool is_stream)
{
  release_stream_input_buffer();

  if (!is_stream) {
    return true;
  }

  uint8_t* psram_buf = static_cast<uint8_t*>(heap_caps_malloc(kMp3StreamInputBufferBytes,
                                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (psram_buf) {
    g_inbuf = psram_buf;
    g_inbuf_capacity = kMp3StreamInputBufferBytes;
    g_inbuf_is_psram = true;
    return true;
  }

  // PSRAM 不够时不要强行占用大量内部 RAM，退回 8KB 安全缓冲，至少保证能播放。
  LOGW("[MP3] 网络流 PSRAM 输入缓冲区分配失败，回退到 %u 字节内部缓冲区",
       (unsigned)sizeof(s_file_inbuf));
  return true;
}

static size_t input_buffer_free_bytes()
{
  if (!g_inbuf || g_inbuf_capacity <= (size_t)g_inbuf_filled) return 0;
  return g_inbuf_capacity - (size_t)g_inbuf_filled;
}

static void reset_decoder_state()
{
  mp3dec_init(&g_dec);
  g_inbuf_filled = 0;
  g_playing = false;
  g_sr = 44100;
  s_last_sr = 0;
  s_channels = 2;
  s_pending_off = 0;
  s_pending_frames = 0;
  g_source_eof = false;
}

static void clear_source()
{
  g_source = AudioMp3Source{};
  g_source_active = false;
  g_source_is_stream = false;
  s_debug_name = nullptr;
  s_mp3_debug_name = String();
}

static bool fill_input_buffer(size_t min_fill_target, uint32_t wait_timeout_ms = 0)
{
  if (!g_source_active || !g_source.read || !g_inbuf) return false;
  if (min_fill_target > g_inbuf_capacity) min_fill_target = g_inbuf_capacity;
  if ((size_t)g_inbuf_filled >= min_fill_target) return true;

  const uint32_t start_ms = millis();
  bool waited = false;

  while ((size_t)g_inbuf_filled < min_fill_target) {
    const size_t space = input_buffer_free_bytes();
    if (space == 0) return true;

    int n = g_source.read(g_source.ctx, g_inbuf + g_inbuf_filled, space);
    if (n > 0) {
      g_inbuf_filled += n;
      continue;
    }

    if (n == AUDIO_MP3_SOURCE_WOULD_BLOCK) {
      if (wait_timeout_ms > 0 && (millis() - start_ms) < wait_timeout_ms) {
        waited = true;
        vTaskDelay(pdMS_TO_TICKS(5));
        continue;
      }
      if (waited) {
        LOGD("[MP3] 网络流预填充不足：目标=%u 已填=%d 等待=%lums",
             (unsigned)min_fill_target,
             g_inbuf_filled,
             (unsigned long)(millis() - start_ms));
      }
      return true;
    }

    if (n == AUDIO_MP3_SOURCE_EOF) {
      g_source_eof = true;
      return true;
    }

    LOGE("[MP3] 音源读取失败：名称=%s 代码=%d", s_debug_name ? s_debug_name : "<null>", n);
    return false;
  }

  return true;
}
}

bool audio_mp3_start_source(const AudioMp3Source& source, const char* debug_name)
{
  audio_mp3_stop();

  if (!source.read) {
    LOGE("[MP3] 无效音源：缺少读取回调");
    return false;
  }

  const uint32_t t0 = millis();
  const uint32_t t_after_init = t0;

  g_source = source;
  g_source_active = true;
  g_source_is_stream = source.is_stream;

  // debug_name 不能直接保存外部指针。
  // NAS 播放时传进来的 url 可能来自 AudioTask 栈上的 AudioCmd.path，
  // 命令处理完后该栈内存会被复用；继续用旧指针打印日志可能造成崩溃。
  s_mp3_debug_name = debug_name
      ? String(debug_name)
      : (source.debug_name ? String(source.debug_name) : String());
  s_debug_name = s_mp3_debug_name.length() ? s_mp3_debug_name.c_str() : nullptr;

  if (!select_input_buffer_for_source(g_source_is_stream)) {
    audio_mp3_stop();
    return false;
  }

  reset_decoder_state();

  const size_t prefill_target = g_source_is_stream ? kMp3StreamStartupPrefillBytes : g_inbuf_capacity;
  const uint32_t prefill_timeout = g_source_is_stream ? kMp3StreamStartupPrefillTimeoutMs : 0;
  if (!fill_input_buffer(prefill_target, prefill_timeout)) {
    audio_mp3_stop();
    return false;
  }

  if (!g_source_is_stream && g_inbuf_filled <= 0) {
    audio_mp3_stop();
    return false;
  }

  if (g_source_is_stream && g_inbuf_filled <= 0) {
    LOGW("[MP3] 网络流以空输入缓冲区启动：名称=%s", s_debug_name ? s_debug_name : "<null>");
  }

  g_playing = true;

  // 设置主线状态
  s_mp3_active = true;
  s_mp3_last_error = String();

  const uint32_t t_after_prefill = millis();
  LOGD("[MP3] 音源启动细节：名称=%s 流=%d 初始化=%lums 预填充=%lums 总计=%lums 预填字节=%d 缓冲=%u PSRAM=%d 目标=%u",
       s_debug_name ? s_debug_name : "<null>",
       g_source_is_stream ? 1 : 0,
       (unsigned long)(t_after_init - t0),
       (unsigned long)(t_after_prefill - t_after_init),
       (unsigned long)(t_after_prefill - t0),
       g_inbuf_filled,
       (unsigned)g_inbuf_capacity,
       g_inbuf_is_psram ? 1 : 0,
       (unsigned)prefill_target);
  return true;
}

bool audio_mp3_is_active() { return s_mp3_active; }
bool audio_mp3_is_stream_source() { return g_source_is_stream; }
uint32_t audio_mp3_get_sample_rate() { return s_mp3_sample_rate; }
uint8_t audio_mp3_get_channels() { return s_mp3_channels; }
uint32_t audio_mp3_get_bitrate_kbps() { return s_mp3_bitrate_kbps; }
const char* audio_mp3_get_last_error() { return s_mp3_last_error.c_str(); }

bool audio_mp3_start_file(SdFat& sd, const char* path)
{
  AudioMp3Source src{};
  if (!audio_mp3_file_source_open(sd, path, src)) {
    return false;
  }

  if (!audio_mp3_start_source(src, path)) {
    audio_mp3_file_source_close();
    return false;
  }

  return true;
}

bool audio_mp3_start_url(const char* url)
{
  AudioMp3Source src{};
  if (!audio_mp3_audiotools_source_open(url, src)) {
    return false;
  }

  if (!audio_mp3_start_source(src, url)) {
    audio_mp3_audiotools_source_close();
    return false;
  }

  return true;
}

void audio_mp3_stop()
{
  s_pending_off = 0;
  s_pending_frames = 0;

  if (g_source_active && g_source.close) {
    g_source.close(g_source.ctx);
  }

  clear_source();
  g_playing = false;
  g_inbuf_filled = 0;
  g_source_eof = false;
  release_stream_input_buffer();

  // 更新主线状态
  s_mp3_active = false;
}

bool audio_mp3_loop()
{
  if (!g_playing) return false;

  // --- A) 先把 pending 的 PCM 写完 ---
  if (s_pending_frames > 0) {
    size_t w = audio_i2s_write_frames(g_pcm + s_pending_off * 2, s_pending_frames);
    if (w == SIZE_MAX) { audio_mp3_stop(); return false; }
    s_pending_off    += w;
    s_pending_frames -= w;
    return true;
  }

  // --- B) 输入补充 ---
  const size_t refill_low = g_source_is_stream ? kMp3StreamRefillLowBytes : 2048;
  const size_t refill_target = g_source_is_stream ? kMp3StreamRefillTargetBytes : 2048;
  if ((size_t)g_inbuf_filled < refill_low) {
    if (!fill_input_buffer(refill_target)) {
      audio_mp3_stop();
      return false;
    }
    if (g_inbuf_filled == 0) {
      if (g_source_eof) {
        audio_mp3_stop();
        return false;
      }
      // 流式输入：暂时没数据，保持播放任务活着
      return true;
    }
  }

  // --- C) 解一帧 ---
  mp3dec_frame_info_t info;
  int samples = mp3dec_decode_frame(&g_dec, g_inbuf, g_inbuf_filled, g_pcm, &info);

  if (info.frame_bytes == 0) {
    if (g_inbuf_filled >= 2) {
      int sync_pos = -1;
      for (int i = 1; i < g_inbuf_filled - 1; ++i) {
        if (g_inbuf[i] == 0xFF && (g_inbuf[i + 1] & 0xE0) == 0xE0) {
          sync_pos = i;
          break;
        }
      }

      if (sync_pos > 0) {
        memmove(g_inbuf, g_inbuf + sync_pos, g_inbuf_filled - sync_pos);
        g_inbuf_filled -= sync_pos;
        LOGD("[MP3] 已重新同步到位置 %d", sync_pos);
      } else {
        int keep = 1;
        memmove(g_inbuf, g_inbuf + g_inbuf_filled - keep, keep);
        g_inbuf_filled = keep;
      }
    }

    // 文件源且已经 EOF：继续冲一轮残余字节后退出；流源则保持等待下一批输入。
    if (g_source_eof && g_inbuf_filled <= 1) {
      audio_mp3_stop();
      return false;
    }
    return true;
  }

  // --- D) 消费输入 ---
  if (info.frame_bytes > 0 && info.frame_bytes <= g_inbuf_filled) {
    memmove(g_inbuf, g_inbuf + info.frame_bytes, g_inbuf_filled - info.frame_bytes);
    g_inbuf_filled -= info.frame_bytes;
  } else {
    audio_mp3_stop();
    return false;
  }

  // --- E) 处理单声道/双声道 ---
  if (samples > 0) {
    g_sr = info.hz;
    s_channels = info.channels;

    // 更新主线状态格式信息
    s_mp3_sample_rate = info.hz;
    s_mp3_channels = info.channels;
    if (info.bitrate_kbps > 0) s_mp3_bitrate_kbps = info.bitrate_kbps;

    if (g_sr != s_last_sr) {
      audio_i2s_set_sample_rate(g_sr);
      s_last_sr = g_sr;
    }

    // 如果是单声道，扩充为双声道（复制到左右声道）
    if (s_channels == 1) {
      // 从后往前复制，避免覆盖
      for (int i = samples - 1; i >= 0; --i) {
        g_pcm[i * 2] = g_pcm[i];     // 左声道
        g_pcm[i * 2 + 1] = g_pcm[i]; // 右声道
      }
    }

    // --- F) 写 PCM（建立 pending） ---
    s_pending_off = 0;
    s_pending_frames = (size_t)samples;// 转换为帧数（每帧2个样本）

    // 先尝试写一次，写不完就留 pending
    size_t w = audio_i2s_write_frames(g_pcm, s_pending_frames);
    if (w == SIZE_MAX) { audio_mp3_stop(); return false; }
    s_pending_off    += w;
    s_pending_frames -= w;
  }

  return true;
}
