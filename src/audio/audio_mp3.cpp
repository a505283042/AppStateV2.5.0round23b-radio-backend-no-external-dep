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
#include "app_diagnostics.h"

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
static AudioPlaybackEndReason s_end_reason = AudioPlaybackEndReason::None;

static void set_end_reason_if_none(AudioPlaybackEndReason reason)
{
  if (s_end_reason == AudioPlaybackEndReason::None) {
    s_end_reason = reason;
  }
}

static void set_last_error(const char* error)
{
  s_mp3_last_error = error ? String(error) : String();
}

static void set_http_source_error_or(const char* fallback)
{
  const char* source_error = audio_mp3_audiotools_source_get_last_error();
  set_last_error((source_error && *source_error) ? source_error : fallback);
}

static constexpr size_t kMp3FileInputBufferBytes = 8 * 1024;
// 网络 MP3 流比本地文件更怕 UI/菜单短时间抢 CPU。
// 这里给网络流单独使用更大的输入缓冲，并优先放到 PSRAM。
static constexpr size_t kMp3StreamInputBufferBytes = 96 * 1024;
static constexpr size_t kMp3StreamStartupPrefillBytes = 48 * 1024;
static constexpr size_t kMp3StreamRefillLowBytes = 48 * 1024;
static constexpr size_t kMp3StreamRefillWaitLowBytes = 32 * 1024;
static constexpr size_t kMp3StreamRefillTargetBytes = 88 * 1024;
static constexpr size_t kMp3StreamMinDecodeBytes = 2048;
static constexpr uint32_t kMp3StreamStartupPrefillTimeoutMs = 3000;
static constexpr uint32_t kMp3StreamRefillWaitTimeoutMs = 25;
static constexpr uint32_t kMp3DiagLogIntervalMs = 2000;
static constexpr uint32_t kMp3DiagLoopGapMs = 60;

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

static uint32_t s_diag_last_loop_ms = 0;
static uint32_t s_diag_last_loop_gap_log_ms = 0;
static uint32_t s_diag_last_wait_log_ms = 0;
static uint32_t s_diag_last_low_log_ms = 0;
static uint32_t s_diag_last_resync_log_ms = 0;
static uint32_t s_diag_loop_gap_events = 0;
static uint32_t s_diag_wait_events = 0;
static uint32_t s_diag_low_events = 0;
static uint32_t s_diag_resync_events = 0;

static bool diag_log_due(uint32_t& last_ms, uint32_t now_ms)
{
  if (last_ms == 0 || now_ms - last_ms >= kMp3DiagLogIntervalMs) {
    last_ms = now_ms;
    return true;
  }
  return false;
}

static void reset_stream_diag_state()
{
  s_diag_last_loop_ms = 0;
  s_diag_last_loop_gap_log_ms = 0;
  s_diag_last_wait_log_ms = 0;
  s_diag_last_low_log_ms = 0;
  s_diag_last_resync_log_ms = 0;
  s_diag_loop_gap_events = 0;
  s_diag_wait_events = 0;
  s_diag_low_events = 0;
  s_diag_resync_events = 0;
}

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
  reset_stream_diag_state();
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
      const uint32_t now_ms = millis();
      if (g_source_is_stream) {
#if APP_DIAG_AUDIO_RUNTIME
        ++s_diag_wait_events;
        if ((size_t)g_inbuf_filled < kMp3StreamRefillWaitLowBytes &&
            diag_log_due(s_diag_last_wait_log_ms, now_ms)) {
          LOGI("[MP3诊断] 网络暂时无数据 events=%lu fill=%d target=%u cap=%u 等待=%lums",
               (unsigned long)s_diag_wait_events,
               g_inbuf_filled,
               (unsigned)min_fill_target,
               (unsigned)g_inbuf_capacity,
               (unsigned long)(now_ms - start_ms));
        }
#endif
      }
      if (wait_timeout_ms > 0 && (now_ms - start_ms) < wait_timeout_ms) {
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

    set_end_reason_if_none(AudioPlaybackEndReason::SourceIoError);
    if (g_source_is_stream) {
      set_http_source_error_or("stream_read_failed");
    } else {
      set_last_error("source_read_failed");
    }
    LOGE("[MP3] 音源读取失败：名称=%s 代码=%d 错误=%s",
         s_debug_name ? s_debug_name : "<null>",
         n,
         s_mp3_last_error.c_str());
    return false;
  }

  return true;
}

static bool prepare_stream_first_frame()
{
  while (g_inbuf_filled >= 2) {
    mp3dec_frame_info_t info{};
    const int samples = mp3dec_decode_frame(&g_dec,
                                            g_inbuf,
                                            g_inbuf_filled,
                                            g_pcm,
                                            &info);

    if (info.frame_bytes <= 0) {
      int sync_pos = -1;
      for (int i = 1; i < g_inbuf_filled - 1; ++i) {
        if (g_inbuf[i] == 0xFF && (g_inbuf[i + 1] & 0xE0) == 0xE0) {
          sync_pos = i;
          break;
        }
      }

      if (sync_pos <= 0) {
        return false;
      }

      memmove(g_inbuf, g_inbuf + sync_pos, g_inbuf_filled - sync_pos);
      g_inbuf_filled -= sync_pos;
      continue;
    }

    if (info.frame_bytes > g_inbuf_filled) {
      set_last_error("stream_invalid_frame_size");
      return false;
    }

    memmove(g_inbuf,
            g_inbuf + info.frame_bytes,
            g_inbuf_filled - info.frame_bytes);
    g_inbuf_filled -= info.frame_bytes;

    // minimp3 可能只跳过无效数据而没有输出 PCM，继续寻找下一帧。
    if (samples <= 0) {
      continue;
    }

    if (info.hz <= 0 || (info.channels != 1 && info.channels != 2)) {
      set_last_error("stream_invalid_audio_format");
      return false;
    }

    g_sr = info.hz;
    s_channels = info.channels;
    s_mp3_sample_rate = info.hz;
    s_mp3_channels = info.channels;
    if (info.bitrate_kbps > 0) {
      s_mp3_bitrate_kbps = info.bitrate_kbps;
    }

    // 第一帧已经解码到软件 PCM 缓冲，必须先配置正确采样率，
    // 否则 audio_mp3_loop() 会在 pending 分支直接按旧采样率写入 I2S。
    if (g_sr != s_last_sr) {
      audio_i2s_set_sample_rate(g_sr);
      s_last_sr = g_sr;
    }

    if (s_channels == 1) {
      for (int i = samples - 1; i >= 0; --i) {
        g_pcm[i * 2] = g_pcm[i];
        g_pcm[i * 2 + 1] = g_pcm[i];
      }
    }

    s_pending_off = 0;
    s_pending_frames = static_cast<size_t>(samples);
    LOGD("[MP3] 网络流首帧验证成功：采样率=%d 声道=%d 码率=%dkbps PCM帧=%d",
         info.hz,
         info.channels,
         info.bitrate_kbps,
         samples);
    return true;
  }

  return false;
}
}

bool audio_mp3_start_source(const AudioMp3Source& source, const char* debug_name)
{
  audio_mp3_stop();
  s_end_reason = AudioPlaybackEndReason::None;
  set_last_error(nullptr);
  s_mp3_sample_rate = 0;
  s_mp3_channels = 0;
  s_mp3_bitrate_kbps = 0;

  if (!source.read) {
    set_last_error("invalid_source");
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
    set_last_error("stream_buffer_alloc_failed");
    audio_mp3_stop();
    return false;
  }

  reset_decoder_state();

  const size_t prefill_target = g_source_is_stream ? kMp3StreamStartupPrefillBytes : g_inbuf_capacity;
  const uint32_t prefill_timeout = g_source_is_stream ? kMp3StreamStartupPrefillTimeoutMs : 0;
  if (!fill_input_buffer(prefill_target, prefill_timeout)) {
    if (s_mp3_last_error.length() == 0) {
      set_last_error(g_source_is_stream ? "stream_prefill_failed" : "source_prefill_failed");
    }
    audio_mp3_stop();
    return false;
  }

  if (!g_source_is_stream && g_inbuf_filled <= 0) {
    set_last_error("empty_file");
    audio_mp3_stop();
    return false;
  }

  if (g_source_is_stream && g_inbuf_filled <= 0) {
    set_end_reason_if_none(AudioPlaybackEndReason::SourceIoError);
    set_last_error(g_source_eof ? "stream_ended_before_audio" : "stream_start_no_data");
    LOGW("[MP3] 网络流起播失败：没有收到音频数据 名称=%s EOF=%d",
         s_debug_name ? s_debug_name : "<null>",
         g_source_eof ? 1 : 0);
    audio_mp3_stop();
    return false;
  }

  if (g_source_is_stream && !prepare_stream_first_frame()) {
    set_end_reason_if_none(AudioPlaybackEndReason::DecodeError);
    if (s_mp3_last_error.length() == 0) {
      set_last_error("stream_invalid_mp3");
    }
    LOGE("[MP3] 网络流起播失败：预填充数据中未找到有效 MP3 帧 名称=%s 字节=%d",
         s_debug_name ? s_debug_name : "<null>",
         g_inbuf_filled);
    audio_mp3_stop();
    return false;
  }

  g_playing = true;

  // 只有已经拿到有效音频帧的网络流，才允许进入 active/playing 状态。
  s_mp3_active = true;

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


bool audio_mp3_is_seekable()
{
  return g_playing && g_source_active &&
      g_source.seek && g_source.tell && g_source.size &&
      g_source.size(g_source.ctx) > g_source.audio_data_offset;
}

static bool prepare_after_seek(uint32_t* out_source_offset)
{
  const size_t prefill_target = g_source_is_stream
      ? kMp3StreamStartupPrefillBytes
      : g_inbuf_capacity;
  const uint32_t prefill_timeout = g_source_is_stream
      ? kMp3StreamStartupPrefillTimeoutMs
      : 0;

  reset_decoder_state();
  if (!fill_input_buffer(prefill_target, prefill_timeout) || g_inbuf_filled <= 0) {
    return false;
  }
  if (!prepare_stream_first_frame()) {
    return false;
  }

  g_playing = true;
  s_mp3_active = true;
  s_end_reason = AudioPlaybackEndReason::None;
  set_last_error(nullptr);
  if (out_source_offset && g_source.tell) {
    const uint32_t tell = g_source.tell(g_source.ctx);
    *out_source_offset = tell >= (uint32_t)g_inbuf_filled
        ? tell - (uint32_t)g_inbuf_filled
        : 0;
  }
  return true;
}

bool audio_mp3_seek_ms(uint32_t target_ms, uint32_t total_ms, uint32_t* out_actual_ms)
{
  if (out_actual_ms) *out_actual_ms = 0;
  if (!audio_mp3_is_seekable() || total_ms == 0) {
    set_last_error("seek_not_supported");
    return false;
  }

  const uint32_t source_size = g_source.size(g_source.ctx);
  const uint32_t audio_begin = g_source.audio_data_offset < source_size
      ? g_source.audio_data_offset
      : 0;
  const uint32_t audio_bytes = source_size - audio_begin;
  if (audio_bytes < 4) {
    set_last_error("seek_source_too_small");
    return false;
  }

  if (target_ms > total_ms) target_ms = total_ms;
  uint64_t scaled = (uint64_t)audio_bytes * (uint64_t)target_ms;
  uint32_t target_offset = audio_begin + (uint32_t)(scaled / total_ms);
  if (target_offset >= source_size) target_offset = source_size - 1;

  const uint32_t source_tell = g_source.tell(g_source.ctx);
  const uint32_t rollback_offset = source_tell >= (uint32_t)g_inbuf_filled
      ? source_tell - (uint32_t)g_inbuf_filled
      : audio_begin;

  // 清除旧位置的压缩数据和 PCM，seek 后重新同步第一帧。
  s_pending_off = 0;
  s_pending_frames = 0;
  g_inbuf_filled = 0;
  g_source_eof = false;

  if (!g_source.seek(g_source.ctx, target_offset)) {
    if (g_source_is_stream) {
      set_http_source_error_or("stream_seek_failed");
    } else {
      set_last_error("file_seek_failed");
    }
    return false;
  }

  uint32_t synced_offset = target_offset;
  if (!prepare_after_seek(&synced_offset)) {
    const String seek_error = s_mp3_last_error;
    LOGW("[MP3] 跳转后帧同步失败，尝试恢复原位置：target=%lu rollback=%lu 错误=%s",
         (unsigned long)target_offset,
         (unsigned long)rollback_offset,
         seek_error.c_str());

    if (g_source.seek(g_source.ctx, rollback_offset) && prepare_after_seek(nullptr)) {
      set_last_error(seek_error.length() ? seek_error.c_str() : "seek_frame_sync_failed");
    } else {
      g_playing = false;
      s_mp3_active = false;
      set_last_error("seek_and_restore_failed");
    }
    return false;
  }

  // CBR 文件通常接近目标位置；无 Xing/VBRI 索引的 VBR 文件属于比例近似。
  const uint32_t actual_ms = target_ms;
  if (out_actual_ms) *out_actual_ms = actual_ms;
  LOGI("[MP3] 跳转成功：目标=%lums 字节=%lu/%lu 同步位置=%lu 来源=%s",
       (unsigned long)target_ms,
       (unsigned long)target_offset,
       (unsigned long)source_size,
       (unsigned long)synced_offset,
       g_source_is_stream ? "NAS" : "本地");
  return true;
}

bool audio_mp3_is_active() { return s_mp3_active; }
bool audio_mp3_is_stream_source() { return g_source_is_stream; }
uint32_t audio_mp3_get_sample_rate() { return s_mp3_sample_rate; }
uint8_t audio_mp3_get_channels() { return s_mp3_channels; }
uint32_t audio_mp3_get_bitrate_kbps() { return s_mp3_bitrate_kbps; }
const char* audio_mp3_get_last_error() { return s_mp3_last_error.c_str(); }
AudioPlaybackEndReason audio_mp3_get_end_reason() { return s_end_reason; }

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

bool audio_mp3_start_url(const char* url, uint32_t operation_id)
{
  // HTTP 打开可能在进入统一解码核心前失败，因此必须先清除上一条流的错误。
  s_end_reason = AudioPlaybackEndReason::None;
  set_last_error(nullptr);

  AudioMp3Source src{};
  if (!audio_mp3_audiotools_source_open(url, operation_id, src)) {
    set_end_reason_if_none(AudioPlaybackEndReason::SourceIoError);
    set_http_source_error_or("stream_open_failed");
    return false;
  }

  if (!audio_mp3_start_source(src, url)) {
    audio_mp3_audiotools_source_close();
    return false;
  }

  return true;
}

bool audio_mp3_start_url_from_offset(const char* url, uint32_t start_offset, uint32_t operation_id)
{
  if (start_offset == 0) {
    return audio_mp3_start_url(url, operation_id);
  }

  // Range 打开失败时也要覆盖旧错误，供 AudioTask 决定是否回退普通 URL。
  s_end_reason = AudioPlaybackEndReason::None;
  set_last_error(nullptr);

  AudioMp3Source src{};
  if (!audio_mp3_audiotools_source_open_from_offset(url, start_offset, operation_id, src)) {
    set_end_reason_if_none(AudioPlaybackEndReason::SourceIoError);
    set_http_source_error_or("stream_open_failed");
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
  if (g_playing && s_end_reason == AudioPlaybackEndReason::None) {
    s_end_reason = AudioPlaybackEndReason::Stopped;
  }

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

  if (g_source_is_stream) {
    const uint32_t now_ms = millis();
#if APP_DIAG_AUDIO_RUNTIME
    if (s_diag_last_loop_ms != 0) {
      const uint32_t gap_ms = now_ms - s_diag_last_loop_ms;
      if (gap_ms >= kMp3DiagLoopGapMs) {
        ++s_diag_loop_gap_events;
        if (diag_log_due(s_diag_last_loop_gap_log_ms, now_ms)) {
          LOGI("[MP3诊断] 解码循环间隔过长 gap=%lums events=%lu fill=%d pending=%u eof=%d",
               (unsigned long)gap_ms,
               (unsigned long)s_diag_loop_gap_events,
               g_inbuf_filled,
               (unsigned)s_pending_frames,
               g_source_eof ? 1 : 0);
        }
      }
    }
#endif
    s_diag_last_loop_ms = now_ms;
  }

  // --- A) 先把 pending 的 PCM 写完 ---
  if (s_pending_frames > 0) {
    size_t w = audio_i2s_write_frames(g_pcm + s_pending_off * 2, s_pending_frames);
    if (w == SIZE_MAX) {
      set_end_reason_if_none(AudioPlaybackEndReason::OutputError);
      set_last_error("i2s_write_failed");
      audio_mp3_stop();
      return false;
    }
    s_pending_off    += w;
    s_pending_frames -= w;
    return true;
  }

  // --- B) 输入补充 ---
  const size_t refill_low = g_source_is_stream ? kMp3StreamRefillLowBytes : 2048;
  const size_t refill_target = g_source_is_stream ? kMp3StreamRefillTargetBytes : 2048;
  if ((size_t)g_inbuf_filled < refill_low) {
    const bool stream_wait_needed = g_source_is_stream &&
                                    (size_t)g_inbuf_filled < kMp3StreamRefillWaitLowBytes;
    const uint32_t refill_wait_ms = stream_wait_needed ? kMp3StreamRefillWaitTimeoutMs : 0;
    if (!fill_input_buffer(refill_target, refill_wait_ms)) {
      audio_mp3_stop();
      return false;
    }
    #if APP_DIAG_AUDIO_RUNTIME
    if (g_source_is_stream && !g_source_eof && (size_t)g_inbuf_filled < refill_low) {
      ++s_diag_low_events;
      const uint32_t now_ms = millis();
      if ((size_t)g_inbuf_filled < kMp3StreamRefillWaitLowBytes &&
          diag_log_due(s_diag_last_low_log_ms, now_ms)) {
        LOGI("[MP3诊断] 网络缓冲低水位 events=%lu fill=%d low=%u hard=%u target=%u cap=%u",
             (unsigned long)s_diag_low_events,
             g_inbuf_filled,
             (unsigned)refill_low,
             (unsigned)kMp3StreamRefillWaitLowBytes,
             (unsigned)refill_target,
             (unsigned)g_inbuf_capacity);
      }
    }
#endif
    if (g_inbuf_filled == 0) {
      if (g_source_eof) {
        set_end_reason_if_none(AudioPlaybackEndReason::NaturalEof);
        audio_mp3_stop();
        return false;
      }
      // 流式输入：暂时没数据，保持播放任务活着
      return true;
    }
  }

  // --- C) 解一帧 ---
  if (g_source_is_stream && !g_source_eof && g_inbuf_filled < (int)kMp3StreamMinDecodeBytes) {
    return true;
  }

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
#if APP_DIAG_AUDIO_RUNTIME
        if (g_source_is_stream) {
          ++s_diag_resync_events;
          const uint32_t now_ms = millis();
          if (diag_log_due(s_diag_last_resync_log_ms, now_ms)) {
            LOGI("[MP3诊断] 流重新同步 pos=%d events=%lu fill=%d",
                 sync_pos,
                 (unsigned long)s_diag_resync_events,
                 g_inbuf_filled);
          }
        }
#endif
        LOGD("[MP3] 已重新同步到位置 %d", sync_pos);
      } else {
        int keep = 1;
        memmove(g_inbuf, g_inbuf + g_inbuf_filled - keep, keep);
        g_inbuf_filled = keep;
      }
    }

    // 文件源且已经 EOF：继续冲一轮残余字节后退出；流源则保持等待下一批输入。
    if (g_source_eof && g_inbuf_filled <= 1) {
      set_end_reason_if_none(AudioPlaybackEndReason::NaturalEof);
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
    set_end_reason_if_none(AudioPlaybackEndReason::DecodeError);
    set_last_error("invalid_mp3_frame_size");
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
    if (w == SIZE_MAX) {
      set_end_reason_if_none(AudioPlaybackEndReason::OutputError);
      set_last_error("i2s_write_failed");
      audio_mp3_stop();
      return false;
    }
    s_pending_off    += w;
    s_pending_frames -= w;
  }

  return true;
}
