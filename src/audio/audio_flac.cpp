#include <Arduino.h>
#include "audio/audio_flac.h"
#include "audio/audio_decode_workspace.h"
#include "audio/audio_i2s.h"
#include "audio/audio_file.h"
#include "audio/audio_http_range_source.h"
#include "board/board_pins.h"
#include "utils/log.h"
#include "app_diagnostics.h"
#include <esp_heap_caps.h>
#if APP_DIAG_NAS_FLAC_PERFORMANCE
#include <esp_timer.h>
#endif
#include <string.h>

// 项目仅使用自定义读写回调播放原生 FLAC；关闭未使用接口和逐帧 CRC，降低高规格文件解码开销。
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#define DR_FLAC_NO_CRC
#define DR_FLAC_IMPLEMENTATION
#include "../../lib/dr_libs/dr_flac.h"

static AudioFile g_file;
static drflac* g_flac = nullptr;
static bool g_playing = false;
static int g_sr = 44100;
static uint32_t g_ch = 2;
static uint32_t s_total_ms = 0;
static size_t s_pending_off = 0;
static size_t s_pending_frames = 0;
static const int16_t* s_pending_pcm = nullptr;
static int s_last_sr = 0; // 上次设置的采样率（文件级 static，便于重置）
static AudioPlaybackEndReason s_end_reason = AudioPlaybackEndReason::None;
static char s_last_error[80] = {0};

enum class FlacSourceKind : uint8_t {
  None = 0,
  LocalFile,
  HttpRange,
};

static FlacSourceKind s_source_kind = FlacSourceKind::None;

static void clear_flac_error()
{
  s_last_error[0] = '\0';
}

static void set_flac_error(const char* error)
{
  if (!error || !*error) {
    clear_flac_error();
    return;
  }
  strncpy(s_last_error, error, sizeof(s_last_error) - 1);
  s_last_error[sizeof(s_last_error) - 1] = '\0';
}

static void set_end_reason_if_none(AudioPlaybackEndReason reason)
{
  if (s_end_reason == AudioPlaybackEndReason::None) {
    s_end_reason = reason;
  }
}

// FLAC 批量解码 4096 frames，减少 dr_flac 调用次数和 bitstream 边界切换。
// 44.1kHz 下约 92ms 音频，配合 NAS 环形缓存可降低解码调度压力。
static constexpr uint32_t FLAC_BUFFER_FRAMES = 4096;
static constexpr uint32_t FLAC_PCM_SAMPLES_PER_CHUNK = FLAC_BUFFER_FRAMES * 2 + 64;
static_assert(FLAC_PCM_SAMPLES_PER_CHUNK <= AudioDecodeWorkspace::kPcmSamples,
              "共享 PCM 工作区不足以容纳 FLAC 单次解码输出");

// 码率用于网络预取配置；真正的单块实时预算由采样率决定。
static uint32_t s_average_bitrate_kbps = 0;
static uint32_t s_pcm_bitrate_kbps = 0;

#if APP_DIAG_NAS_FLAC_PERFORMANCE
static constexpr uint32_t kDecodeDiagSummaryIntervalMs = 5000;
static constexpr uint32_t kDecodeOverrunLogIntervalMs = 2000;
static uint32_t s_decode_diag_started_ms = 0;
static uint32_t s_decode_diag_last_overrun_log_ms = 0;
static uint32_t s_decode_diag_samples = 0;
static uint32_t s_decode_diag_overruns = 0;
static uint64_t s_decode_diag_total_us = 0;
static uint64_t s_decode_diag_source_wait_us = 0;
static uint32_t s_decode_diag_source_wait_chunks = 0;
static uint32_t s_decode_diag_last_reader_wait_count = 0;
static uint32_t s_decode_diag_last_low_watermark_count = 0;
static uint32_t s_decode_diag_max_us = 0;
static uint32_t s_decode_diag_max_load_percent = 0;
#endif

// 开播前软件 PCM 缓冲：只预解码，不写 I2S。
// 之前直接预填 I2S DMA 时，I2S 硬件会在功放静音期间把开头音频播放掉，
// 所以无法形成“开声后的缓冲余量”。这里改为先把 PCM 放在 RAM，开声后再快速写入 I2S。
static constexpr uint8_t FLAC_PRIME_CHUNKS = 8;
static int16_t* s_prime_pcm = nullptr;
static uint16_t s_prime_frames[FLAC_PRIME_CHUNKS] = {0};
static uint8_t s_prime_head = 0;
static uint8_t s_prime_count = 0;

static size_t prime_buffer_bytes()
{
  return (size_t)FLAC_PRIME_CHUNKS * FLAC_PCM_SAMPLES_PER_CHUNK * sizeof(int16_t);
}

static int16_t* prime_chunk_ptr(uint8_t idx)
{
  if (!s_prime_pcm || idx >= FLAC_PRIME_CHUNKS) return nullptr;
  return s_prime_pcm + ((size_t)idx * FLAC_PCM_SAMPLES_PER_CHUNK);
}

static bool ensure_prime_buffer()
{
  if (s_prime_pcm) return true;

  const size_t bytes = prime_buffer_bytes();
  void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) {
    LOGW("[FLAC] 启动软件预填充 PSRAM 分配失败，跳过预填充 size=%lu",
         (unsigned long)bytes);
    return false;
  }

  s_prime_pcm = static_cast<int16_t*>(p);
  LOGD("[FLAC] 启动软件预填充缓冲 已分配=%lu 字节 PSRAM=%d",
       (unsigned long)bytes,
       esp_ptr_external_ram(s_prime_pcm) ? 1 : 0);
  return true;
}

static void clear_prime_buffer()
{
  s_prime_head = 0;
  s_prime_count = 0;
  for (uint8_t i = 0; i < FLAC_PRIME_CHUNKS; ++i) {
    s_prime_frames[i] = 0;
  }
}

static ssize_t source_read(void* buffer_out, size_t bytes_to_read)
{
  if (s_source_kind == FlacSourceKind::LocalFile) {
    return g_file.read(buffer_out, bytes_to_read);
  }
  if (s_source_kind == FlacSourceKind::HttpRange) {
    return audio_http_range_source_read(buffer_out, bytes_to_read);
  }
  return -1;
}

static bool source_seek(uint32_t absolute_offset)
{
  if (s_source_kind == FlacSourceKind::LocalFile) {
    return g_file.seek(absolute_offset);
  }
  if (s_source_kind == FlacSourceKind::HttpRange) {
    return audio_http_range_source_seek(absolute_offset);
  }
  return false;
}

static uint32_t source_tell()
{
  if (s_source_kind == FlacSourceKind::LocalFile) return g_file.tell();
  if (s_source_kind == FlacSourceKind::HttpRange) return audio_http_range_source_tell();
  return 0;
}

static uint32_t source_size()
{
  if (s_source_kind == FlacSourceKind::LocalFile) return g_file.size();
  if (s_source_kind == FlacSourceKind::HttpRange) return audio_http_range_source_size();
  return 0;
}

static bool source_had_io_error()
{
  if (s_source_kind == FlacSourceKind::LocalFile) return g_file.had_io_error();
  if (s_source_kind == FlacSourceKind::HttpRange) return audio_http_range_source_had_io_error();
  return true;
}

static void source_close()
{
  if (s_source_kind == FlacSourceKind::LocalFile) {
    if (g_file.f) g_file.close();
  } else if (s_source_kind == FlacSourceKind::HttpRange) {
    audio_http_range_source_close();
  }
  s_source_kind = FlacSourceKind::None;
}

static const char* source_last_error()
{
  if (s_source_kind == FlacSourceKind::HttpRange) {
    return audio_http_range_source_get_last_error();
  }
  return s_last_error;
}

static size_t on_read(void*, void* buffer_out, size_t bytes_to_read)
{
  const ssize_t count = source_read(buffer_out, bytes_to_read);
  if (count < 0) {
    const char* error = source_last_error();
    set_flac_error(error && *error ? error : "flac_source_read_failed");
    if (error && strcmp(error, "cancelled") == 0) {
      set_end_reason_if_none(AudioPlaybackEndReason::Stopped);
    } else {
      set_end_reason_if_none(AudioPlaybackEndReason::SourceIoError);
    }
    return 0;
  }
  return static_cast<size_t>(count);
}

static drflac_bool32 on_seek(void*, int offset, drflac_seek_origin origin)
{
  // offset 是 int（可为负），不能直接转成 uint32_t；否则会发生溢出。
  const int64_t current = static_cast<int64_t>(source_tell());
  const int64_t size = static_cast<int64_t>(source_size());
  int64_t base = 0;

  switch (origin) {
    case DRFLAC_SEEK_SET: base = 0; break;
    case DRFLAC_SEEK_CUR: base = current; break;
    case DRFLAC_SEEK_END: base = size; break;
    default: return DRFLAC_FALSE;
  }

  int64_t target = base + static_cast<int64_t>(offset);
  if (target < 0) target = 0;
  if (target > size) target = size;

  if (!source_seek(static_cast<uint32_t>(target))) {
    const char* error = source_last_error();
    set_flac_error(error && *error ? error : "flac_source_seek_failed");
    if (error && strcmp(error, "cancelled") == 0) {
      set_end_reason_if_none(AudioPlaybackEndReason::Stopped);
    } else {
      set_end_reason_if_none(AudioPlaybackEndReason::SourceIoError);
    }
    return DRFLAC_FALSE;
  }
  return DRFLAC_TRUE;
}

static drflac_bool32 on_tell(void*, drflac_int64* cursor)
{
  if (!cursor) return DRFLAC_FALSE;
  *cursor = static_cast<drflac_int64>(source_tell());
  return DRFLAC_TRUE;
}

static void reset_decode_diagnostics()
{
#if APP_DIAG_NAS_FLAC_PERFORMANCE
  s_decode_diag_started_ms = millis();
  s_decode_diag_last_overrun_log_ms = 0;
  s_decode_diag_samples = 0;
  s_decode_diag_overruns = 0;
  s_decode_diag_total_us = 0;
  s_decode_diag_source_wait_us = 0;
  s_decode_diag_source_wait_chunks = 0;
  s_decode_diag_last_reader_wait_count = 0;
  s_decode_diag_last_low_watermark_count = 0;
  s_decode_diag_max_us = 0;
  s_decode_diag_max_load_percent = 0;

  if (s_source_kind == FlacSourceKind::HttpRange) {
    AudioHttpRangeSourceSnapshot snapshot{};
    if (audio_http_range_source_get_snapshot(&snapshot)) {
      s_decode_diag_last_reader_wait_count = snapshot.reader_wait_count;
      s_decode_diag_last_low_watermark_count = snapshot.low_watermark_count;
    }
  }
#endif
}

static uint32_t pcm_duration_us(uint32_t frames)
{
  if (g_sr <= 0 || frames == 0) return 0;
  const uint64_t value = (static_cast<uint64_t>(frames) * 1000000ULL) /
                         static_cast<uint32_t>(g_sr);
  return value > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(value);
}

#if APP_DIAG_NAS_FLAC_PERFORMANCE
static void log_network_decode_performance(uint32_t frames_read,
                                             uint32_t decode_us,
                                             uint32_t source_wait_us,
                                             const AudioHttpRangeSourceSnapshot& snapshot)
{
  if (s_source_kind != FlacSourceKind::HttpRange || frames_read == 0) return;

  const uint32_t audio_us = pcm_duration_us(frames_read);
  const uint32_t pure_decode_us = decode_us > source_wait_us
      ? decode_us - source_wait_us
      : 0;
  const uint32_t load_percent = audio_us > 0
      ? static_cast<uint32_t>((static_cast<uint64_t>(decode_us) * 100ULL) / audio_us)
      : 0;
  ++s_decode_diag_samples;
  s_decode_diag_total_us += decode_us;
  s_decode_diag_source_wait_us += source_wait_us;
  if (source_wait_us > 0) ++s_decode_diag_source_wait_chunks;
  if (decode_us > s_decode_diag_max_us) s_decode_diag_max_us = decode_us;
  if (load_percent > s_decode_diag_max_load_percent) s_decode_diag_max_load_percent = load_percent;
  if (audio_us > 0 && decode_us >= audio_us) ++s_decode_diag_overruns;

  const uint32_t now = millis();
  const bool overrun = audio_us > 0 && decode_us >= audio_us;
  if (overrun && (s_decode_diag_last_overrun_log_ms == 0 ||
      (uint32_t)(now - s_decode_diag_last_overrun_log_ms) >= kDecodeOverrunLogIntervalMs)) {
    s_decode_diag_last_overrun_log_ms = now;
    LOGW("[NAS FLAC] 解码超过实时预算：总计=%lu.%03lums 网络等待=%lu.%03lums 纯解码约=%lu.%03lums 音频=%lu.%03lums 负载=%lu%% frames=%lu 缓存=%lu/%luB",
         (unsigned long)(decode_us / 1000U), (unsigned long)(decode_us % 1000U),
         (unsigned long)(source_wait_us / 1000U), (unsigned long)(source_wait_us % 1000U),
         (unsigned long)(pure_decode_us / 1000U), (unsigned long)(pure_decode_us % 1000U),
         (unsigned long)(audio_us / 1000U), (unsigned long)(audio_us % 1000U),
         (unsigned long)load_percent, (unsigned long)frames_read,
         (unsigned long)snapshot.cached_bytes,
         (unsigned long)snapshot.cache_capacity_bytes);
  }

  if (s_decode_diag_started_ms == 0) s_decode_diag_started_ms = now;
  if ((uint32_t)(now - s_decode_diag_started_ms) < kDecodeDiagSummaryIntervalMs) return;

  const uint32_t avg_total_us = s_decode_diag_samples > 0
      ? static_cast<uint32_t>(s_decode_diag_total_us / s_decode_diag_samples) : 0;
  const uint32_t avg_wait_us = s_decode_diag_samples > 0
      ? static_cast<uint32_t>(s_decode_diag_source_wait_us / s_decode_diag_samples) : 0;
  const uint32_t avg_pure_us = avg_total_us > avg_wait_us
      ? avg_total_us - avg_wait_us
      : 0;
  const uint32_t wait_ratio = s_decode_diag_total_us > 0
      ? static_cast<uint32_t>((s_decode_diag_source_wait_us * 100ULL) / s_decode_diag_total_us)
      : 0;
  const uint32_t wait_events = snapshot.reader_wait_count >= s_decode_diag_last_reader_wait_count
      ? snapshot.reader_wait_count - s_decode_diag_last_reader_wait_count
      : snapshot.reader_wait_count;
  const uint32_t low_water_events = snapshot.low_watermark_count >= s_decode_diag_last_low_watermark_count
      ? snapshot.low_watermark_count - s_decode_diag_last_low_watermark_count
      : snapshot.low_watermark_count;
  const uint32_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const uint32_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  LOGI("[NAS FLAC性能] 平均总计=%lu.%03lums 纯解码约=%lu.%03lums 网络等待均值=%lu.%03lums 等待占比=%lu%% 等待块=%lu/%lu 等待事件=%lu 低水位=%lu 最低缓存=%luB 峰值=%lu.%03lums 超预算=%lu/%lu 负载峰值=%lu%% 当前缓存=%lu/%luB RAM=%lu 最大连续=%lu",
       (unsigned long)(avg_total_us / 1000U), (unsigned long)(avg_total_us % 1000U),
       (unsigned long)(avg_pure_us / 1000U), (unsigned long)(avg_pure_us % 1000U),
       (unsigned long)(avg_wait_us / 1000U), (unsigned long)(avg_wait_us % 1000U),
       (unsigned long)wait_ratio,
       (unsigned long)s_decode_diag_source_wait_chunks,
       (unsigned long)s_decode_diag_samples,
       (unsigned long)wait_events,
       (unsigned long)low_water_events,
       (unsigned long)snapshot.min_cached_bytes,
       (unsigned long)(s_decode_diag_max_us / 1000U), (unsigned long)(s_decode_diag_max_us % 1000U),
       (unsigned long)s_decode_diag_overruns, (unsigned long)s_decode_diag_samples,
       (unsigned long)s_decode_diag_max_load_percent,
       (unsigned long)snapshot.cached_bytes, (unsigned long)snapshot.cache_capacity_bytes,
       (unsigned long)free_internal, (unsigned long)largest_internal);

  s_decode_diag_started_ms = now;
  s_decode_diag_samples = 0;
  s_decode_diag_overruns = 0;
  s_decode_diag_total_us = 0;
  s_decode_diag_source_wait_us = 0;
  s_decode_diag_source_wait_chunks = 0;
  s_decode_diag_last_reader_wait_count = snapshot.reader_wait_count;
  s_decode_diag_last_low_watermark_count = snapshot.low_watermark_count;
  s_decode_diag_max_us = 0;
  s_decode_diag_max_load_percent = 0;
}
#endif

static uint32_t decode_one_chunk_to(int16_t* out_pcm)
{
  if (!g_playing || !g_flac || !out_pcm) return 0;

#if APP_DIAG_NAS_FLAC_PERFORMANCE
  AudioHttpRangeSourceSnapshot before_snapshot{};
  if (s_source_kind == FlacSourceKind::HttpRange) {
    (void)audio_http_range_source_get_snapshot(&before_snapshot);
  }
  const int64_t decode_start_us = esp_timer_get_time();
#endif
  uint32_t frames_read = 0;
  if (g_ch == 2) {
    frames_read = drflac_read_pcm_frames_s16(g_flac, FLAC_BUFFER_FRAMES, out_pcm);
  } else { // g_ch == 1（已在 start 中验证过）
    // 单声道扩充：先读到 out_pcm 的前半（mono），再从后往前扩成 stereo
    frames_read = drflac_read_pcm_frames_s16(g_flac, FLAC_BUFFER_FRAMES, out_pcm);
    if (frames_read > 0) {
      for (int i = (int)frames_read - 1; i >= 0; --i) {
        int16_t v = out_pcm[i];
        out_pcm[i * 2 + 0] = v;
        out_pcm[i * 2 + 1] = v;
      }
    }
  }

#if APP_DIAG_NAS_FLAC_PERFORMANCE
  const int64_t decode_elapsed_us = esp_timer_get_time() - decode_start_us;
  const uint32_t decode_cost_us = decode_elapsed_us > 0
      ? (decode_elapsed_us > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(decode_elapsed_us))
      : 0;

  if (s_source_kind == FlacSourceKind::HttpRange) {
    AudioHttpRangeSourceSnapshot after_snapshot{};
    (void)audio_http_range_source_get_snapshot(&after_snapshot);
    const uint64_t wait_delta = after_snapshot.reader_wait_total_us >= before_snapshot.reader_wait_total_us
        ? after_snapshot.reader_wait_total_us - before_snapshot.reader_wait_total_us
        : after_snapshot.reader_wait_total_us;
    const uint32_t source_wait_us = wait_delta > UINT32_MAX
        ? UINT32_MAX
        : static_cast<uint32_t>(wait_delta);
    log_network_decode_performance(frames_read, decode_cost_us, source_wait_us, after_snapshot);
  }
#endif

  return frames_read;
}

static bool finish_flac_start(const char* debug_name, uint32_t started_ms)
{
  const uint32_t source_opened_ms = millis();
  g_flac = drflac_open(on_read, on_seek, on_tell, nullptr, nullptr);
  if (!g_flac) {
    const char* source_error = source_last_error();
    set_flac_error(source_error && *source_error
        ? source_error
        : "flac_decoder_open_failed");
    LOGE("[FLAC] dr_flac 打开失败：来源=%s 错误=%s",
         debug_name ? debug_name : "<unknown>",
         s_last_error);
    source_close();
    return false;
  }

  const uint32_t decoder_opened_ms = millis();
  g_sr = static_cast<int>(g_flac->sampleRate);
  g_ch = g_flac->channels;
  if (g_ch == 0 || g_ch > 2 || g_sr <= 0) {
    set_flac_error("flac_format_unsupported");
    LOGE("[FLAC] 不支持的格式：采样率=%d 声道=%u", g_sr, (unsigned)g_ch);
    drflac_close(g_flac);
    g_flac = nullptr;
    source_close();
    return false;
  }

  if (g_flac->totalPCMFrameCount > 0) {
    const uint64_t total_ms =
        (static_cast<uint64_t>(g_flac->totalPCMFrameCount) * 1000ULL) /
        static_cast<uint32_t>(g_sr);
    s_total_ms = total_ms > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(total_ms);
  } else {
    s_total_ms = 0;
  }

  const uint32_t file_size = source_size();
  s_average_bitrate_kbps = (file_size > 0 && s_total_ms > 0)
      ? static_cast<uint32_t>((static_cast<uint64_t>(file_size) * 8ULL) / s_total_ms)
      : 0;
  s_pcm_bitrate_kbps = static_cast<uint32_t>(
      (static_cast<uint64_t>(g_sr) * static_cast<uint32_t>(g_ch) *
       static_cast<uint32_t>(g_flac->bitsPerSample)) / 1000ULL);
  if (s_source_kind == FlacSourceKind::HttpRange) {
    audio_http_range_source_set_flac_profile(s_average_bitrate_kbps,
                                             static_cast<uint32_t>(g_sr),
                                             g_flac->bitsPerSample);
  }

  s_last_sr = 0;
  g_playing = true;
  s_pending_pcm = nullptr;
  s_pending_off = 0;
  s_pending_frames = 0;
  clear_prime_buffer();
  clear_flac_error();
  reset_decode_diagnostics();

  const uint32_t chunk_audio_us = pcm_duration_us(FLAC_BUFFER_FRAMES);
  LOGI("[FLAC] 启动成功：来源=%s 类型=%s 文件=%luB 采样率=%d 位深=%u 声道=%u 最大块=%u 平均码率=%luKbps 原始PCM=%luKbps 解码块预算=%lu.%03lums 时长=%lums 源打开=%lums 解码器打开=%lums 总计=%lums",
       debug_name ? debug_name : "<unknown>",
       s_source_kind == FlacSourceKind::HttpRange ? "HTTP Range" : "本地",
       (unsigned long)file_size,
       g_sr,
       (unsigned)g_flac->bitsPerSample,
       (unsigned)g_ch,
       (unsigned)g_flac->maxBlockSizeInPCMFrames,
       (unsigned long)s_average_bitrate_kbps,
       (unsigned long)s_pcm_bitrate_kbps,
       (unsigned long)(chunk_audio_us / 1000U),
       (unsigned long)(chunk_audio_us % 1000U),
       (unsigned long)s_total_ms,
       (unsigned long)(source_opened_ms - started_ms),
       (unsigned long)(decoder_opened_ms - source_opened_ms),
       (unsigned long)(decoder_opened_ms - started_ms));

  if (s_source_kind == FlacSourceKind::HttpRange &&
      (s_average_bitrate_kbps >= 2500 || g_sr >= 88200 || g_flac->bitsPerSample >= 24)) {
    LOGI("[NAS FLAC] 高规格流：码率=%luKbps 采样率=%dHz 位深=%u；4096帧实时预算=%lu.%03lums，已启用发布优化、高水位预取和I2S抗抖动",
         (unsigned long)s_average_bitrate_kbps, g_sr, (unsigned)g_flac->bitsPerSample,
         (unsigned long)(chunk_audio_us / 1000U),
         (unsigned long)(chunk_audio_us % 1000U));
  }
  return true;
}

bool audio_flac_start(SdFat& sd, const char* path)
{
  audio_flac_stop();
  s_end_reason = AudioPlaybackEndReason::None;
  clear_flac_error();
  s_total_ms = 0;
  const uint32_t started_ms = millis();

  if (!path || !g_file.open(sd, path)) {
    set_flac_error("flac_file_open_failed");
    LOGE("[FLAC] 本地文件打开失败：%s", path ? path : "<null>");
    return false;
  }

  s_source_kind = FlacSourceKind::LocalFile;
  return finish_flac_start(path, started_ms);
}

bool audio_flac_start_url(const char* url, uint32_t operation_id)
{
  audio_flac_stop();
  s_end_reason = AudioPlaybackEndReason::None;
  clear_flac_error();
  s_total_ms = 0;
  const uint32_t started_ms = millis();

  if (!url || !*url) {
    set_flac_error("invalid_url");
    return false;
  }

  if (!audio_http_range_source_open(url, operation_id)) {
    const char* source_error = audio_http_range_source_get_last_error();
    set_flac_error(source_error && *source_error
        ? source_error
        : "flac_http_open_failed");
    LOGE("[NAS FLAC] HTTP Range 音源打开失败：错误=%s URL=%s",
         s_last_error,
         url);
    audio_http_range_source_close();
    return false;
  }

  s_source_kind = FlacSourceKind::HttpRange;
  return finish_flac_start(url, started_ms);
}

void audio_flac_stop()
{
  if (g_playing && s_end_reason == AudioPlaybackEndReason::None) {
    s_end_reason = AudioPlaybackEndReason::Stopped;
  }

  // ✅ 清 pending PCM（非常重要）
  s_pending_off = 0;
  s_pending_frames = 0;
  s_pending_pcm = nullptr;
  clear_prime_buffer();

  if (g_flac) { drflac_close(g_flac); g_flac = nullptr; }
  source_close();
  g_playing = false;
  s_total_ms = 0;
  s_average_bitrate_kbps = 0;
  s_pcm_bitrate_kbps = 0;
  reset_decode_diagnostics();
}

AudioPlaybackEndReason audio_flac_get_end_reason()
{
  return s_end_reason;
}

bool audio_flac_is_active()
{
  return g_playing && g_flac != nullptr;
}

bool audio_flac_is_network_source()
{
  return s_source_kind == FlacSourceKind::HttpRange;
}

uint32_t audio_flac_get_sample_rate()
{
  return g_sr > 0 ? static_cast<uint32_t>(g_sr) : 0;
}

uint8_t audio_flac_get_channels()
{
  return static_cast<uint8_t>(g_ch);
}

uint32_t audio_flac_get_total_ms()
{
  return s_total_ms;
}

bool audio_flac_is_seekable()
{
  return g_playing && g_flac && g_sr > 0 && g_flac->totalPCMFrameCount > 0;
}

bool audio_flac_seek_ms(uint32_t target_ms, uint32_t* out_actual_ms)
{
  if (out_actual_ms) *out_actual_ms = 0;
  if (!audio_flac_is_seekable()) {
    set_flac_error("seek_not_supported");
    return false;
  }

  const uint64_t total_frames = g_flac->totalPCMFrameCount;
  uint64_t target_frame = (static_cast<uint64_t>(target_ms) *
                           static_cast<uint32_t>(g_sr)) / 1000ULL;
  if (target_frame >= total_frames) {
    target_frame = total_frames > 0 ? total_frames - 1 : 0;
  }

  // seek 前必须丢弃旧位置的 PCM，包括启动预填充和 I2S pending。
  s_pending_off = 0;
  s_pending_frames = 0;
  s_pending_pcm = nullptr;
  clear_prime_buffer();

  if (drflac_seek_to_pcm_frame(g_flac, target_frame) == DRFLAC_FALSE) {
    const char* source_error = source_last_error();
    set_flac_error(source_error && *source_error
        ? source_error
        : "flac_seek_failed");
    LOGW("[FLAC] 跳转失败：目标=%lums frame=%llu 来源=%s 错误=%s",
         (unsigned long)target_ms,
         (unsigned long long)target_frame,
         s_source_kind == FlacSourceKind::HttpRange ? "NAS" : "本地",
         s_last_error);
    return false;
  }

  s_end_reason = AudioPlaybackEndReason::None;
  clear_flac_error();
  const uint32_t actual_ms = static_cast<uint32_t>(
      (target_frame * 1000ULL) / static_cast<uint32_t>(g_sr));
  if (out_actual_ms) *out_actual_ms = actual_ms;
  LOGI("[FLAC] 跳转成功：目标=%lums 实际=%lums frame=%llu 来源=%s",
       (unsigned long)target_ms,
       (unsigned long)actual_ms,
       (unsigned long long)target_frame,
       s_source_kind == FlacSourceKind::HttpRange ? "NAS" : "本地");
  return true;
}

const char* audio_flac_get_last_error()
{
  if (s_last_error[0]) return s_last_error;
  const char* source_error = source_last_error();
  return source_error ? source_error : "";
}

uint32_t audio_flac_prime_pcm_ms(uint32_t target_ms, uint32_t max_chunks)
{
  if (!g_playing || !g_flac) return 0;
  if (target_ms == 0 || max_chunks == 0) return 0;
  if (max_chunks > FLAC_PRIME_CHUNKS) max_chunks = FLAC_PRIME_CHUNKS;

  // 新文件第一次真正写 I2S 前先设置采样率；这里只改时钟，不写 PCM。
  if (g_sr != s_last_sr) {
    audio_i2s_set_sample_rate(g_sr);
    s_last_sr = g_sr;
  }

  if (!ensure_prime_buffer()) {
    clear_prime_buffer();
    if (s_source_kind == FlacSourceKind::HttpRange) {
      audio_http_range_source_reset_playback_diagnostics();
    }
    reset_decode_diagnostics();
    return 0;
  }

  clear_prime_buffer();

  const uint32_t t0 = millis();
  uint32_t total_frames = 0;
  uint32_t chunks = 0;
  uint32_t max_decode_ms = 0;

  while (chunks < max_chunks) {
    const uint32_t now_ms = (g_sr > 0) ? ((total_frames * 1000UL) / (uint32_t)g_sr) : 0;
    if (now_ms >= target_ms) break;

    const uint32_t idx = (s_prime_head + s_prime_count) % FLAC_PRIME_CHUNKS;
    int16_t* chunk_pcm = prime_chunk_ptr((uint8_t)idx);
    if (!chunk_pcm) {
      LOGW("[FLAC] 启动软件预填充缓冲无效 idx=%u", (unsigned)idx);
      break;
    }

    const uint32_t td0 = millis();
    const uint32_t frames = decode_one_chunk_to(chunk_pcm);
    const uint32_t decode_ms = millis() - td0;
    if (decode_ms > max_decode_ms) max_decode_ms = decode_ms;

    if (frames == 0) {
      break;
    }

    s_prime_frames[idx] = (uint16_t)frames;
    ++s_prime_count;
    ++chunks;
    total_frames += frames;
  }

  const uint32_t primed_ms = (g_sr > 0) ? ((total_frames * 1000UL) / (uint32_t)g_sr) : 0;
  if (s_source_kind == FlacSourceKind::HttpRange) {
    // 启动阶段会主动消费网络缓存；从预填充结束后再统计最低缓存，避免固定显示0B。
    audio_http_range_source_reset_playback_diagnostics();
  }
  // 详细诊断开启时，开播后的首个统计窗口不再包含软件预解码阶段。
  reset_decode_diagnostics();
  LOGD("[FLAC] 启动软件预填充：时长=%lums 块=%lu 帧=%lu 耗时=%lums 最大解码=%lums",
       (unsigned long)primed_ms,
       (unsigned long)chunks,
       (unsigned long)total_frames,
       (unsigned long)(millis() - t0),
       (unsigned long)max_decode_ms);
  return primed_ms;
}

static bool write_pending_pcm()
{
  if (s_pending_frames == 0 || !s_pending_pcm) return true;

  size_t w = audio_i2s_write_frames(s_pending_pcm + s_pending_off * 2, s_pending_frames);
  if (w == SIZE_MAX) {
    set_end_reason_if_none(AudioPlaybackEndReason::OutputError);
    audio_flac_stop();
    return false;
  }
  s_pending_off += w;
  s_pending_frames -= w;
  if (s_pending_frames == 0) {
    s_pending_off = 0;
    s_pending_pcm = nullptr;
  }
  return true;
}

bool audio_flac_loop()
{
  if (!g_playing || !g_flac) return false;

  // A) 先写完 pending
  if (s_pending_frames > 0) {
    return write_pending_pcm();
  }

  // B) 优先把开播前软件预解码的 PCM 写入 I2S。
  // 这一步不读 TF、不解码，只把 RAM 中的 PCM 推给 DMA，给 FLAC 后续慢解码留余量。
  if (s_prime_count > 0) {
    const uint8_t idx = s_prime_head;
    s_prime_head = (s_prime_head + 1) % FLAC_PRIME_CHUNKS;
    --s_prime_count;

    s_pending_pcm = prime_chunk_ptr(idx);
    if (!s_pending_pcm) {
      s_pending_off = 0;
      s_pending_frames = 0;
      s_prime_frames[idx] = 0;
      clear_prime_buffer();
      return true;
    }
    s_pending_off = 0;
    s_pending_frames = s_prime_frames[idx];
    s_prime_frames[idx] = 0;
    return write_pending_pcm();
  }

  // C) 读新 PCM（按 channels 读）
  uint32_t frames_read = decode_one_chunk_to(AudioDecodeWorkspace::pcm);

  if (frames_read == 0) {
    if (source_had_io_error()) {
      set_end_reason_if_none(AudioPlaybackEndReason::SourceIoError);
    } else if (source_tell() >= source_size()) {
      set_end_reason_if_none(AudioPlaybackEndReason::NaturalEof);
    } else {
      set_end_reason_if_none(AudioPlaybackEndReason::DecodeError);
    }

    if (s_end_reason == AudioPlaybackEndReason::NaturalEof) {
      LOGD("[FLAC] 播放结束：原因=%s 文件位置=%lu/%lu",
           audio_playback_end_reason_label(s_end_reason),
           (unsigned long)source_tell(),
           (unsigned long)source_size());
    } else {
      LOGW("[FLAC] 播放异常结束：原因=%s 文件位置=%lu/%lu 错误=%s",
           audio_playback_end_reason_label(s_end_reason),
           (unsigned long)source_tell(),
           (unsigned long)source_size(),
           audio_flac_get_last_error());
    }
    audio_flac_stop();
    return false;
  }

  // D) 设置采样率（不要重 init）
  if (g_sr != s_last_sr) {
    audio_i2s_set_sample_rate(g_sr);
    s_last_sr = g_sr;
  }

  // E) 建 pending 并尝试写
  s_pending_pcm = AudioDecodeWorkspace::pcm;
  s_pending_off = 0;
  s_pending_frames = frames_read;

  size_t w = audio_i2s_write_frames(s_pending_pcm, s_pending_frames);
  if (w == SIZE_MAX) {
    set_end_reason_if_none(AudioPlaybackEndReason::OutputError);
    audio_flac_stop();
    return false;
  }
  s_pending_off += w;
  s_pending_frames -= w;
  if (s_pending_frames == 0) {
    s_pending_off = 0;
    s_pending_pcm = nullptr;
  }

  return true;
}
