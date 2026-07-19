#include <Arduino.h>
#include "audio/audio_flac.h"
#include "audio/audio_i2s.h"
#include "audio/audio_file.h"
#include "audio/audio_http_range_source.h"
#include "board/board_pins.h"
#include "utils/log.h"
#include <esp_heap_caps.h>
#include <string.h>

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

// FLAC 每次解码 1024 frames，44.1k 下约 23ms。
static constexpr uint32_t FLAC_BUFFER_FRAMES = 1024;
static constexpr uint32_t FLAC_PCM_SAMPLES_PER_CHUNK = FLAC_BUFFER_FRAMES * 2 + 64;
static int16_t s_decode_pcm[FLAC_PCM_SAMPLES_PER_CHUNK]; // stereo buffer + 安全边距

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

static uint32_t decode_one_chunk_to(int16_t* out_pcm)
{
  if (!g_playing || !g_flac || !out_pcm) return 0;

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

  s_last_sr = 0;
  g_playing = true;
  s_pending_pcm = nullptr;
  s_pending_off = 0;
  s_pending_frames = 0;
  clear_prime_buffer();
  clear_flac_error();

  LOGI("[FLAC] 启动成功：来源=%s 类型=%s 文件=%luB 采样率=%d 声道=%u 时长=%lums 源打开=%lums 解码器打开=%lums 总计=%lums",
       debug_name ? debug_name : "<unknown>",
       s_source_kind == FlacSourceKind::HttpRange ? "HTTP Range" : "本地",
       (unsigned long)source_size(),
       g_sr,
       (unsigned)g_ch,
       (unsigned long)s_total_ms,
       (unsigned long)(source_opened_ms - started_ms),
       (unsigned long)(decoder_opened_ms - source_opened_ms),
       (unsigned long)(decoder_opened_ms - started_ms));
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
  uint32_t frames_read = decode_one_chunk_to(s_decode_pcm);

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
  s_pending_pcm = s_decode_pcm;
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
