// Unified MP3 decode core
// 职责：
// 1) 维护统一的 MP3 解码后半段：inbuf / minimp3 decode / resync / PCM pending / I2S 输出
// 2) 不关心输入来自文件还是网络
// 3) 文件和网络输入都通过 AudioMp3Source 适配接入

#include <Arduino.h>
#include "audio/audio_mp3.h"
#include "audio/audio_i2s.h"
#include "audio/audio_file.h"
#include "audio/audio_mp3_source_file.h"
#include "audio/audio_mp3_source_audiotools.h"
#include "utils/log.h"

#define MINIMP3_IMPLEMENTATION
#include "../../lib/minimp3/minimp3.h"

namespace {
static SdFat* g_sd = nullptr;
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

static uint8_t g_inbuf[8 * 1024];
static int g_inbuf_filled = 0;
static bool g_playing = false;
static int g_sr = 44100;

static int16_t g_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME * 2]; // stereo interleaved (预留双声道空间)
static size_t s_pending_off = 0;
static size_t s_pending_frames = 0;
static int s_channels = 2; // 当前声道数
static int s_last_sr = 0; // 上次设置的采样率（文件级 static，便于重置）
static const char* s_debug_name = nullptr;

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
}

static bool fill_input_buffer(size_t min_fill_target)
{
  if (!g_source_active || !g_source.read) return false;
  if ((size_t)g_inbuf_filled >= min_fill_target) return true;

  int space = (int)sizeof(g_inbuf) - g_inbuf_filled;
  if (space <= 0) return true;

  int n = g_source.read(g_source.ctx, g_inbuf + g_inbuf_filled, (size_t)space);
  if (n > 0) {
    g_inbuf_filled += n;
    return true;
  }

  if (n == AUDIO_MP3_SOURCE_WOULD_BLOCK) {
    return true;
  }

  if (n == AUDIO_MP3_SOURCE_EOF) {
    g_source_eof = true;
    return true;
  }

  LOGE("[MP3] source read failed name=%s code=%d", s_debug_name ? s_debug_name : "<null>", n);
  return false;
}
}

bool audio_mp3_start_source(const AudioMp3Source& source, const char* debug_name)
{
  audio_mp3_stop();

  if (!source.read) {
    LOGE("[MP3] invalid source: read callback missing");
    return false;
  }

  const uint32_t t0 = millis();
  const uint32_t t_after_init = t0;

  g_source = source;
  g_source_active = true;
  g_source_is_stream = source.is_stream;
  s_debug_name = debug_name ? debug_name : source.debug_name;
  reset_decoder_state();

  // 对本地文件保持"启动即预读"的行为；对网络流允许先空转等待。
  if (!fill_input_buffer(sizeof(g_inbuf))) {
    audio_mp3_stop();
    return false;
  }

  if (!g_source_is_stream && g_inbuf_filled <= 0) {
    audio_mp3_stop();
    return false;
  }

  g_playing = true;
  
  // 设置主线状态
  s_mp3_active = true;
  s_mp3_debug_name = debug_name ? String(debug_name) : String();
  s_mp3_last_error = String();
  
  const uint32_t t_after_prefill = millis();
  LOGI("[MP3] start source detail name=%s stream=%d init=%lums prefill=%lums total=%lums prefill_bytes=%d",
       s_debug_name ? s_debug_name : "<null>",
       g_source_is_stream ? 1 : 0,
       (unsigned long)(t_after_init - t0),
       (unsigned long)(t_after_prefill - t_after_init),
       (unsigned long)(t_after_prefill - t0),
       g_inbuf_filled);
  return true;
}

bool audio_mp3_is_active() { return s_mp3_active; }
bool audio_mp3_is_stream_source() { return g_source_is_stream; }
uint32_t audio_mp3_get_sample_rate() { return s_mp3_sample_rate; }
uint8_t audio_mp3_get_channels() { return s_mp3_channels; }
uint32_t audio_mp3_get_bitrate_kbps() { return s_mp3_bitrate_kbps; }
const char* audio_mp3_get_last_error() { return s_mp3_last_error.c_str(); }
const char* audio_mp3_get_debug_name() { return s_mp3_debug_name.c_str(); }

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
  if (g_inbuf_filled < 2048) {
    if (!fill_input_buffer(2048)) {
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
        LOGD("[MP3] Resynced to pos %d", sync_pos);
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