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

enum class Mp3SeekStrategy : uint8_t {
  Proportional = 0,
  XingToc,
  VbriToc,
  CbrBitrate
};

enum class Mp3VbrHeaderKind : uint8_t {
  None = 0,
  Xing,
  Info,
  Vbri
};

struct Mp3SeekIndexState {
  bool first_frame_found = false;
  uint32_t first_frame_offset = 0;
  uint32_t first_bitrate_kbps = 0;
  uint32_t first_sample_rate = 0;
  uint32_t xing_frames = 0;
  uint32_t xing_stream_bytes = 0;
  uint32_t xing_total_ms = 0;
  uint32_t vbri_stream_bytes = 0;
  uint32_t vbri_frames = 0;
  uint32_t vbri_total_ms = 0;
  uint16_t vbri_entry_count = 0;
  uint16_t vbri_frames_per_entry = 0;
  uint32_t vbri_indexed_bytes = 0;
  uint32_t* vbri_cumulative_bytes = nullptr;
  Mp3VbrHeaderKind header_kind = Mp3VbrHeaderKind::None;
  bool toc_valid = false;
  uint8_t toc[100] = {0};
  uint16_t same_bitrate_frames = 0;
  bool bitrate_varied = false;
  bool cbr_confirmed = false;
  bool cbr_revoke_logged = false;
  bool xing_size_mismatch = false;
  uint32_t repaired_total_ms = 0;
};

static Mp3SeekIndexState s_seek_index{};
static uint32_t s_prepared_frame_offset = 0;
static constexpr uint16_t kMp3CbrConfirmFrames = 48;
static constexpr uint32_t kMp3HeaderSizeAbsoluteToleranceBytes = 4096;
static constexpr uint32_t kMp3HeaderSizeRelativeToleranceDivisor = 100; // 允许约 1% 的尾部差异
// 前置声明，避免辅助函数定义位于首帧解析函数之后导致编译失败。
static void validate_xing_size_against_source();

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

static uint16_t read_be16(const uint8_t* p)
{
  if (!p) return 0;
  return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t read_be32(const uint8_t* p)
{
  if (!p) return 0;
  return ((uint32_t)p[0] << 24) |
         ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) |
         (uint32_t)p[3];
}

static uint32_t read_be_variable(const uint8_t* p, uint8_t bytes)
{
  if (!p || bytes == 0 || bytes > 4) return 0;
  uint32_t value = 0;
  for (uint8_t i = 0; i < bytes; ++i) {
    value = (value << 8) | p[i];
  }
  return value;
}

static const char* seek_strategy_label(Mp3SeekStrategy strategy)
{
  switch (strategy) {
    case Mp3SeekStrategy::XingToc: return "Xing/Info TOC";
    case Mp3SeekStrategy::VbriToc: return "VBRI TOC";
    case Mp3SeekStrategy::CbrBitrate: return "CBR码率";
    case Mp3SeekStrategy::Proportional:
    default: return "字节比例";
  }
}

static void reset_seek_index_state()
{
  if (s_seek_index.vbri_cumulative_bytes) {
    heap_caps_free(s_seek_index.vbri_cumulative_bytes);
  }
  s_seek_index = Mp3SeekIndexState{};
  s_prepared_frame_offset = 0;
}

static uint32_t current_input_buffer_offset()
{
  if (!g_source_active || !g_source.tell) {
    return g_source.audio_data_offset;
  }

  const uint32_t tell = g_source.tell(g_source.ctx);
  return tell >= (uint32_t)g_inbuf_filled
      ? tell - (uint32_t)g_inbuf_filled
      : 0;
}

struct Mp3FrameHeaderSummary {
  size_t xing_offset = 0;
  uint32_t samples_per_frame = 0;
  uint32_t bitrate_kbps = 0;
  uint32_t sample_rate = 0;
  uint32_t frame_bytes = 0;
  uint8_t channels = 0;
};

static uint32_t mp3_header_bitrate_kbps(uint32_t version, uint32_t bitrate_index)
{
  static const uint16_t kMpeg1Layer3[16] = {
      0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
  static const uint16_t kMpeg2Layer3[16] = {
      0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
  if (bitrate_index >= 16u) return 0;
  return version == 3u ? kMpeg1Layer3[bitrate_index] : kMpeg2Layer3[bitrate_index];
}

static uint32_t mp3_header_sample_rate(uint32_t version, uint32_t sample_rate_index)
{
  static const uint16_t kMpeg1[3] = {44100, 48000, 32000};
  static const uint16_t kMpeg2[3] = {22050, 24000, 16000};
  static const uint16_t kMpeg25[3] = {11025, 12000, 8000};
  if (sample_rate_index >= 3u) return 0;
  if (version == 3u) return kMpeg1[sample_rate_index];
  if (version == 2u) return kMpeg2[sample_rate_index];
  if (version == 0u) return kMpeg25[sample_rate_index];
  return 0;
}

static bool parse_mp3_frame_header(const uint8_t* frame,
                                   size_t available_bytes,
                                   Mp3FrameHeaderSummary* out)
{
  if (out) *out = Mp3FrameHeaderSummary{};
  if (!frame || available_bytes < 4 || !out) return false;

  const uint32_t h = read_be32(frame);
  if ((h & 0xFFE00000u) != 0xFFE00000u) return false;

  const uint32_t version = (h >> 19) & 0x3u;
  const uint32_t layer = (h >> 17) & 0x3u;
  const bool protection_absent = ((h >> 16) & 0x1u) != 0;
  const uint32_t bitrate_index = (h >> 12) & 0xFu;
  const uint32_t sample_rate_index = (h >> 10) & 0x3u;
  const uint32_t padding = (h >> 9) & 0x1u;
  const uint32_t channel_mode = (h >> 6) & 0x3u;

  if (version == 1u || layer != 1u ||
      bitrate_index == 0u || bitrate_index == 0xFu ||
      sample_rate_index == 3u) {
    return false;
  }

  const uint32_t bitrate_kbps = mp3_header_bitrate_kbps(version, bitrate_index);
  const uint32_t sample_rate = mp3_header_sample_rate(version, sample_rate_index);
  if (bitrate_kbps == 0 || sample_rate == 0) return false;

  const bool mono = channel_mode == 3u;
  const size_t side_info_bytes = version == 3u
      ? (mono ? 17u : 32u)
      : (mono ? 9u : 17u);
  const size_t crc_bytes = protection_absent ? 0u : 2u;
  const size_t xing_offset = 4u + crc_bytes + side_info_bytes;
  const uint32_t frame_bytes = version == 3u
      ? (144000u * bitrate_kbps / sample_rate) + padding
      : (72000u * bitrate_kbps / sample_rate) + padding;
  if (frame_bytes < 4u ||
      xing_offset + 8u > frame_bytes ||
      xing_offset + 8u > available_bytes) {
    return false;
  }

  out->xing_offset = xing_offset;
  out->samples_per_frame = version == 3u ? 1152u : 576u;
  out->bitrate_kbps = bitrate_kbps;
  out->sample_rate = sample_rate;
  out->frame_bytes = frame_bytes;
  out->channels = mono ? 1u : 2u;
  return true;
}

static bool validate_xing_toc(const uint8_t* toc)
{
  if (!toc) return false;

  bool has_progress = false;
  uint8_t previous = toc[0];
  for (size_t i = 1; i < 100; ++i) {
    if (toc[i] < previous) return false;
    if (toc[i] > previous) has_progress = true;
    previous = toc[i];
  }
  return has_progress && toc[99] > 0;
}

static bool inspect_vbri_seek_metadata(const uint8_t* frame,
                                       size_t frame_bytes,
                                       uint32_t absolute_offset,
                                       const Mp3FrameHeaderSummary& header)
{
  // VBRI 固定在 MPEG 音频头结束后 32 字节，即帧起点 + 4 + 32。
  static constexpr size_t kVbriOffset = 4u + 32u;
  static constexpr size_t kVbriFixedBytes = 26u;
  static constexpr uint16_t kMaxVbriEntries = 1024u;

  if (!frame || frame_bytes < kVbriOffset + kVbriFixedBytes) return false;
  const uint8_t* vbri = frame + kVbriOffset;
  if (memcmp(vbri, "VBRI", 4) != 0) return false;

  // 已识别到 VBRI 标记后，即使索引字段损坏或内存不足，也不能再误判为 CBR。
  s_seek_index.header_kind = Mp3VbrHeaderKind::Vbri;

  const uint16_t version = read_be16(vbri + 4u);
  const uint32_t stream_bytes = read_be32(vbri + 10u);
  const uint32_t total_frames = read_be32(vbri + 14u);
  const uint16_t entry_count = read_be16(vbri + 18u);
  const uint16_t scale = read_be16(vbri + 20u);
  const uint16_t entry_bytes = read_be16(vbri + 22u);
  const uint16_t frames_per_entry = read_be16(vbri + 24u);
  const size_t table_bytes = (size_t)entry_count * (size_t)entry_bytes;

  const bool fields_valid = version == 1u &&
      stream_bytes > 0u && total_frames > 0u &&
      entry_count > 0u && entry_count <= kMaxVbriEntries &&
      scale > 0u && entry_bytes >= 1u && entry_bytes <= 4u &&
      frames_per_entry > 0u &&
      kVbriOffset + kVbriFixedBytes + table_bytes <= frame_bytes;
  if (!fields_valid) {
    LOGW("[MP3] VBRI 索引无效：版本=%u 字节=%lu 帧=%lu 条目=%u 缩放=%u 条目字节=%u 每段帧=%u 帧大小=%u 来源=%s",
         (unsigned)version,
         (unsigned long)stream_bytes,
         (unsigned long)total_frames,
         (unsigned)entry_count,
         (unsigned)scale,
         (unsigned)entry_bytes,
         (unsigned)frames_per_entry,
         (unsigned)frame_bytes,
         g_source_is_stream ? "NAS" : "本地");
    return false;
  }

  const size_t cumulative_bytes = ((size_t)entry_count + 1u) * sizeof(uint32_t);
  uint32_t* cumulative = static_cast<uint32_t*>(
      heap_caps_malloc(cumulative_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!cumulative) {
    cumulative = static_cast<uint32_t*>(
        heap_caps_malloc(cumulative_bytes, MALLOC_CAP_8BIT));
  }
  if (!cumulative) {
    LOGW("[MP3] VBRI 索引内存分配失败：条目=%u 字节=%u",
         (unsigned)entry_count,
         (unsigned)cumulative_bytes);
    return false;
  }

  cumulative[0] = 0;
  const uint8_t* table = vbri + kVbriFixedBytes;
  bool table_valid = true;
  bool table_has_progress = false;
  for (uint16_t i = 0; i < entry_count; ++i) {
    const uint32_t raw = read_be_variable(table + (size_t)i * entry_bytes,
                                          (uint8_t)entry_bytes);
    const uint64_t segment_bytes = (uint64_t)raw * (uint64_t)scale;
    const uint64_t next = (uint64_t)cumulative[i] + segment_bytes;
    if (next > UINT32_MAX) {
      table_valid = false;
      break;
    }
    if (segment_bytes > 0u) table_has_progress = true;
    cumulative[i + 1u] = (uint32_t)next;
  }

  if (!table_valid || !table_has_progress || cumulative[entry_count] == 0u) {
    heap_caps_free(cumulative);
    LOGW("[MP3] VBRI TOC 条目无效：条目=%u 来源=%s",
         (unsigned)entry_count,
         g_source_is_stream ? "NAS" : "本地");
    return false;
  }

  s_seek_index.vbri_stream_bytes = stream_bytes;
  s_seek_index.vbri_frames = total_frames;
  s_seek_index.vbri_entry_count = entry_count;
  s_seek_index.vbri_frames_per_entry = frames_per_entry;
  s_seek_index.vbri_indexed_bytes = cumulative[entry_count];
  s_seek_index.vbri_cumulative_bytes = cumulative;
  if (header.samples_per_frame > 0u && header.sample_rate > 0u) {
    const uint64_t samples =
        (uint64_t)total_frames * (uint64_t)header.samples_per_frame;
    s_seek_index.vbri_total_ms =
        (uint32_t)(samples * 1000ull / (uint64_t)header.sample_rate);
  }

  LOGI("[MP3] 跳转索引：类型=VBRI TOC=1 帧=%lu 字节=%lu 时长=%lums 条目=%u 每段帧=%u 索引字节=%lu 首帧=%lu 来源=%s",
       (unsigned long)s_seek_index.vbri_frames,
       (unsigned long)s_seek_index.vbri_stream_bytes,
       (unsigned long)s_seek_index.vbri_total_ms,
       (unsigned)s_seek_index.vbri_entry_count,
       (unsigned)s_seek_index.vbri_frames_per_entry,
       (unsigned long)s_seek_index.vbri_indexed_bytes,
       (unsigned long)absolute_offset,
       g_source_is_stream ? "NAS" : "本地");
  return true;
}

static void inspect_first_frame_seek_metadata(const uint8_t* frame,
                                              size_t frame_bytes,
                                              uint32_t absolute_offset,
                                              const mp3dec_frame_info_t& info)
{
  if (s_seek_index.first_frame_found || !frame || frame_bytes < 4) return;

  Mp3FrameHeaderSummary header{};
  if (!parse_mp3_frame_header(frame, frame_bytes, &header)) return;
  const size_t xing_offset = header.xing_offset;
  const uint32_t samples_per_frame = header.samples_per_frame;

  s_seek_index.first_frame_found = true;
  s_seek_index.first_frame_offset = absolute_offset;
  s_seek_index.first_bitrate_kbps = info.bitrate_kbps > 0
      ? (uint32_t)info.bitrate_kbps
      : 0;
  s_seek_index.first_sample_rate = info.hz > 0
      ? (uint32_t)info.hz
      : 0;

  const uint8_t* tag = frame + xing_offset;
  if (memcmp(tag, "Xing", 4) == 0) {
    s_seek_index.header_kind = Mp3VbrHeaderKind::Xing;
  } else if (memcmp(tag, "Info", 4) == 0) {
    s_seek_index.header_kind = Mp3VbrHeaderKind::Info;
  } else {
    if (inspect_vbri_seek_metadata(frame, frame_bytes, absolute_offset, header)) {
      return;
    }
    if (s_seek_index.header_kind == Mp3VbrHeaderKind::Vbri) {
      LOGW("[MP3] 已发现 VBRI，但索引不可用，回退字节比例定位 来源=%s 首帧=%lu",
           g_source_is_stream ? "NAS" : "本地",
           (unsigned long)absolute_offset);
      return;
    }
    LOGD("[MP3] 跳转索引：未发现 Xing/Info/VBRI，开始观察码率 来源=%s 首帧=%lu 码率=%lukbps",
         g_source_is_stream ? "NAS" : "本地",
         (unsigned long)absolute_offset,
         (unsigned long)s_seek_index.first_bitrate_kbps);
    return;
  }

  const uint32_t flags = read_be32(tag + 4);
  size_t cursor = xing_offset + 8u;
  bool fields_valid = true;

  if (flags & 0x0001u) {
    if (cursor + 4u > frame_bytes) fields_valid = false;
    else {
      s_seek_index.xing_frames = read_be32(frame + cursor);
      cursor += 4u;
    }
  }
  if (fields_valid && (flags & 0x0002u)) {
    if (cursor + 4u > frame_bytes) fields_valid = false;
    else {
      s_seek_index.xing_stream_bytes = read_be32(frame + cursor);
      cursor += 4u;
    }
  }
  if (fields_valid && (flags & 0x0004u)) {
    if (cursor + sizeof(s_seek_index.toc) > frame_bytes) fields_valid = false;
    else {
      memcpy(s_seek_index.toc, frame + cursor, sizeof(s_seek_index.toc));
      s_seek_index.toc_valid = validate_xing_toc(s_seek_index.toc);
      cursor += sizeof(s_seek_index.toc);
    }
  }
  if (fields_valid && (flags & 0x0008u)) {
    if (cursor + 4u > frame_bytes) fields_valid = false;
    else cursor += 4u;
  }

  if (s_seek_index.xing_frames > 0 &&
      samples_per_frame > 0 &&
      s_seek_index.first_sample_rate > 0) {
    const uint64_t total_samples =
        (uint64_t)s_seek_index.xing_frames * (uint64_t)samples_per_frame;
    s_seek_index.xing_total_ms = (uint32_t)(
        total_samples * 1000ull / s_seek_index.first_sample_rate);
  }

  if (s_seek_index.header_kind == Mp3VbrHeaderKind::Info) {
    // LAME 的 Info 标记用于 CBR 文件；后续逐帧观察仍会在码率变化时撤销。
    s_seek_index.cbr_confirmed = s_seek_index.first_bitrate_kbps > 0;
  }

  validate_xing_size_against_source();

  const char* fields_state = !fields_valid
      ? "截断"
      : (s_seek_index.xing_size_mismatch ? "源大小不一致" : "正常");

  LOGI("[MP3] 跳转索引：类型=%s TOC=%d 帧=%lu 字节=%lu 时长=%lums 首帧=%lu 码率=%lukbps 字段=%s 来源=%s",
       s_seek_index.header_kind == Mp3VbrHeaderKind::Info ? "Info" : "Xing",
       s_seek_index.toc_valid ? 1 : 0,
       (unsigned long)s_seek_index.xing_frames,
       (unsigned long)s_seek_index.xing_stream_bytes,
       (unsigned long)s_seek_index.xing_total_ms,
       (unsigned long)s_seek_index.first_frame_offset,
       (unsigned long)s_seek_index.first_bitrate_kbps,
       fields_state,
       g_source_is_stream ? "NAS" : "本地");
}

static void validate_xing_size_against_source()
{
  if (!g_source_active || !g_source.size ||
      !s_seek_index.first_frame_found ||
      s_seek_index.xing_stream_bytes < 4u) {
    return;
  }

  const uint32_t source_size = g_source.size(g_source.ctx);
  if (source_size <= s_seek_index.first_frame_offset) return;

  const uint32_t actual_audio_bytes =
      source_size - s_seek_index.first_frame_offset;
  const uint32_t declared_audio_bytes = s_seek_index.xing_stream_bytes;
  if (declared_audio_bytes <= actual_audio_bytes) return;

  const uint32_t missing_bytes = declared_audio_bytes - actual_audio_bytes;
  uint32_t tolerance_bytes =
      actual_audio_bytes / kMp3HeaderSizeRelativeToleranceDivisor;
  if (tolerance_bytes < kMp3HeaderSizeAbsoluteToleranceBytes) {
    tolerance_bytes = kMp3HeaderSizeAbsoluteToleranceBytes;
  }
  if (missing_bytes <= tolerance_bytes) return;

  // 文件被截断或 Info/Xing 头来自旧文件时，TOC 和帧数都对应原始长度，
  // 继续使用会造成跳转位置和总时长同时偏大。
  s_seek_index.xing_size_mismatch = true;
  s_seek_index.toc_valid = false;

  if (s_seek_index.header_kind == Mp3VbrHeaderKind::Info &&
      s_seek_index.first_bitrate_kbps > 0u) {
    // Info 标记代表 CBR；kbps × ms / 8 = 字节，因此反推时长为 字节 × 8 / kbps。
    const uint64_t repaired_ms =
        (uint64_t)actual_audio_bytes * 8ull /
        (uint64_t)s_seek_index.first_bitrate_kbps;
    if (repaired_ms > 0u && repaired_ms <= UINT32_MAX) {
      s_seek_index.repaired_total_ms = (uint32_t)repaired_ms;
    }
  }

  LOGW("[MP3] %s 头与实际文件大小不一致：声明音频=%lu 实际音频=%lu 缺少=%lu 容差=%lu，TOC已禁用 修正时长=%lums 来源=%s",
       s_seek_index.header_kind == Mp3VbrHeaderKind::Info ? "Info" : "Xing",
       (unsigned long)declared_audio_bytes,
       (unsigned long)actual_audio_bytes,
       (unsigned long)missing_bytes,
       (unsigned long)tolerance_bytes,
       (unsigned long)s_seek_index.repaired_total_ms,
       g_source_is_stream ? "NAS" : "本地");
}

static void probe_initial_seek_metadata()
{
  if (s_seek_index.first_frame_found || !g_inbuf || g_inbuf_filled < 4) return;

  const uint32_t buffer_offset = current_input_buffer_offset();
  const int scan_limit = g_inbuf_filled < 4096 ? g_inbuf_filled : 4096;
  for (int i = 0; i <= scan_limit - 4; ++i) {
    if (g_inbuf[i] != 0xFF || (g_inbuf[i + 1] & 0xE0) != 0xE0) continue;

    Mp3FrameHeaderSummary header{};
    if (!parse_mp3_frame_header(g_inbuf + i,
                                (size_t)(g_inbuf_filled - i),
                                &header) ||
        header.frame_bytes > (uint32_t)(g_inbuf_filled - i)) {
      continue;
    }

    // 初始探测不消费解码器状态，因此额外验证下一帧头，避免标签或封面数据中的伪同步字。
    const int next_offset = i + (int)header.frame_bytes;
    if (next_offset + 4 <= g_inbuf_filled) {
      Mp3FrameHeaderSummary next{};
      if (!parse_mp3_frame_header(g_inbuf + next_offset,
                                  (size_t)(g_inbuf_filled - next_offset),
                                  &next) ||
          next.sample_rate != header.sample_rate ||
          next.channels != header.channels) {
        continue;
      }
    }

    mp3dec_frame_info_t info{};
    info.frame_bytes = (int)header.frame_bytes;
    info.channels = header.channels;
    info.hz = (int)header.sample_rate;
    info.layer = 3;
    info.bitrate_kbps = (int)header.bitrate_kbps;
    inspect_first_frame_seek_metadata(g_inbuf + i,
                                      header.frame_bytes,
                                      buffer_offset + (uint32_t)i,
                                      info);
    return;
  }
}

static void observe_frame_bitrate(uint32_t bitrate_kbps)
{
  if (!s_seek_index.first_frame_found || bitrate_kbps == 0) return;

  if (s_seek_index.first_bitrate_kbps == 0) {
    s_seek_index.first_bitrate_kbps = bitrate_kbps;
  }

  if (bitrate_kbps != s_seek_index.first_bitrate_kbps) {
    s_seek_index.bitrate_varied = true;
    s_seek_index.same_bitrate_frames = 0;
    if (s_seek_index.cbr_confirmed && !s_seek_index.cbr_revoke_logged) {
      LOGW("[MP3] 检测到码率变化，撤销 CBR 精确定位：首帧=%lukbps 当前=%lukbps 来源=%s",
           (unsigned long)s_seek_index.first_bitrate_kbps,
           (unsigned long)bitrate_kbps,
           g_source_is_stream ? "NAS" : "本地");
      s_seek_index.cbr_revoke_logged = true;
    }
    s_seek_index.cbr_confirmed = false;
    return;
  }

  if (s_seek_index.same_bitrate_frames < UINT16_MAX) {
    ++s_seek_index.same_bitrate_frames;
  }

  // Xing 表示 VBR；没有 TOC 时仍使用比例回退，不能因为开头若干帧相同而误判 CBR。
  if (s_seek_index.header_kind == Mp3VbrHeaderKind::Xing ||
      s_seek_index.header_kind == Mp3VbrHeaderKind::Vbri ||
      s_seek_index.bitrate_varied ||
      s_seek_index.cbr_confirmed) {
    return;
  }

  if (s_seek_index.same_bitrate_frames >= kMp3CbrConfirmFrames) {
    s_seek_index.cbr_confirmed = true;
    LOGI("[MP3] CBR 码率确认：码率=%lukbps 连续帧=%u 来源=%s",
         (unsigned long)s_seek_index.first_bitrate_kbps,
         (unsigned)s_seek_index.same_bitrate_frames,
         g_source_is_stream ? "NAS" : "本地");
  }
}

static uint32_t usable_audio_stream_bytes(uint32_t source_size, uint32_t audio_begin)
{
  if (source_size <= audio_begin) return 0;
  const uint32_t source_audio_bytes = source_size - audio_begin;
  if (s_seek_index.xing_stream_bytes >= 4u &&
      s_seek_index.xing_stream_bytes <= source_audio_bytes) {
    return s_seek_index.xing_stream_bytes;
  }
  if (s_seek_index.vbri_stream_bytes >= 4u &&
      s_seek_index.vbri_stream_bytes <= source_audio_bytes) {
    return s_seek_index.vbri_stream_bytes;
  }
  return source_audio_bytes;
}

static Mp3SeekStrategy select_seek_strategy()
{
  if (s_seek_index.first_frame_found && s_seek_index.toc_valid) {
    return Mp3SeekStrategy::XingToc;
  }
  if (s_seek_index.first_frame_found &&
      s_seek_index.header_kind == Mp3VbrHeaderKind::Vbri &&
      s_seek_index.vbri_cumulative_bytes &&
      s_seek_index.vbri_entry_count > 0u &&
      s_seek_index.vbri_frames > 0u) {
    return Mp3SeekStrategy::VbriToc;
  }
  if (s_seek_index.first_frame_found &&
      s_seek_index.cbr_confirmed &&
      !s_seek_index.bitrate_varied &&
      s_seek_index.first_bitrate_kbps > 0) {
    return Mp3SeekStrategy::CbrBitrate;
  }
  return Mp3SeekStrategy::Proportional;
}

static uint32_t calculate_seek_offset(Mp3SeekStrategy strategy,
                                      uint32_t target_ms,
                                      uint32_t total_ms,
                                      uint32_t source_size,
                                      uint32_t audio_begin)
{
  const uint32_t stream_bytes = usable_audio_stream_bytes(source_size, audio_begin);
  if (stream_bytes == 0 || total_ms == 0) return audio_begin;

  uint64_t relative_bytes = 0;
  if (strategy == Mp3SeekStrategy::XingToc) {
    const uint64_t percent_milli =
        (uint64_t)target_ms * 100000ull / (uint64_t)total_ms;
    const uint32_t index = (uint32_t)(percent_milli / 1000ull);
    const uint32_t fraction = (uint32_t)(percent_milli % 1000ull);

    if (index >= 100u) {
      relative_bytes = stream_bytes;
    } else {
      const uint32_t a = s_seek_index.toc[index];
      const uint32_t b = index < 99u ? s_seek_index.toc[index + 1u] : 256u;
      const uint64_t toc_milli =
          (uint64_t)a * 1000ull + (uint64_t)(b - a) * fraction;
      relative_bytes =
          (uint64_t)stream_bytes * toc_milli / (256ull * 1000ull);
    }
  } else if (strategy == Mp3SeekStrategy::VbriToc) {
    const uint32_t total_frames = s_seek_index.vbri_frames;
    const uint16_t entry_count = s_seek_index.vbri_entry_count;
    const uint16_t frames_per_entry = s_seek_index.vbri_frames_per_entry;
    const uint32_t* cumulative = s_seek_index.vbri_cumulative_bytes;
    if (total_frames > 0u && entry_count > 0u && frames_per_entry > 0u && cumulative) {
      uint64_t target_frame =
          (uint64_t)target_ms * (uint64_t)total_frames / (uint64_t)total_ms;
      if (target_frame >= total_frames) target_frame = total_frames - 1u;
      uint32_t entry = (uint32_t)(target_frame / frames_per_entry);
      if (entry >= entry_count) entry = entry_count - 1u;
      const uint64_t entry_first_frame = (uint64_t)entry * frames_per_entry;
      const uint32_t remaining_frames = total_frames > entry_first_frame
          ? (uint32_t)(total_frames - entry_first_frame)
          : 0u;
      const uint32_t entry_frames = remaining_frames < frames_per_entry
          ? remaining_frames
          : (uint32_t)frames_per_entry;
      const uint32_t frame_in_entry =
          (uint32_t)(target_frame - entry_first_frame);
      const uint32_t segment_bytes = cumulative[entry + 1u] - cumulative[entry];
      relative_bytes = cumulative[entry];
      if (entry_frames > 0u) {
        relative_bytes +=
            (uint64_t)segment_bytes * frame_in_entry / entry_frames;
      }
      if (s_seek_index.vbri_indexed_bytes > 0u &&
          s_seek_index.vbri_indexed_bytes != stream_bytes) {
        relative_bytes = relative_bytes * stream_bytes /
            s_seek_index.vbri_indexed_bytes;
      }
    } else {
      relative_bytes =
          (uint64_t)stream_bytes * (uint64_t)target_ms / (uint64_t)total_ms;
    }
  } else if (strategy == Mp3SeekStrategy::CbrBitrate) {
    // kbps × ms / 8 正好得到字节数。
    relative_bytes =
        (uint64_t)s_seek_index.first_bitrate_kbps * (uint64_t)target_ms / 8ull;
  } else {
    relative_bytes =
        (uint64_t)stream_bytes * (uint64_t)target_ms / (uint64_t)total_ms;
  }

  if (relative_bytes >= stream_bytes) relative_bytes = stream_bytes - 1u;
  uint64_t absolute = (uint64_t)audio_begin + relative_bytes;
  if (absolute >= source_size) absolute = source_size - 1u;
  return (uint32_t)absolute;
}

static uint32_t estimate_seek_actual_ms(Mp3SeekStrategy strategy,
                                        uint32_t synced_offset,
                                        uint32_t total_ms,
                                        uint32_t source_size,
                                        uint32_t audio_begin)
{
  if (synced_offset <= audio_begin || total_ms == 0 || source_size <= audio_begin) return 0;

  const uint32_t stream_bytes = usable_audio_stream_bytes(source_size, audio_begin);
  if (stream_bytes == 0) return 0;
  uint32_t relative = synced_offset - audio_begin;
  if (relative > stream_bytes) relative = stream_bytes;

  if (strategy == Mp3SeekStrategy::CbrBitrate &&
      s_seek_index.first_bitrate_kbps > 0) {
    const uint64_t ms = (uint64_t)relative * 8ull / s_seek_index.first_bitrate_kbps;
    return ms > total_ms ? total_ms : (uint32_t)ms;
  }

  if (strategy == Mp3SeekStrategy::VbriToc &&
      s_seek_index.vbri_cumulative_bytes &&
      s_seek_index.vbri_entry_count > 0u &&
      s_seek_index.vbri_frames_per_entry > 0u &&
      s_seek_index.vbri_frames > 0u) {
    const uint32_t* cumulative = s_seek_index.vbri_cumulative_bytes;
    const uint16_t count = s_seek_index.vbri_entry_count;
    const uint16_t frames_per_entry = s_seek_index.vbri_frames_per_entry;
    uint32_t table_relative = relative;
    if (s_seek_index.vbri_indexed_bytes > 0u &&
        stream_bytes > 0u &&
        s_seek_index.vbri_indexed_bytes != stream_bytes) {
      table_relative = (uint32_t)((uint64_t)relative *
          s_seek_index.vbri_indexed_bytes / stream_bytes);
    }
    if (table_relative > s_seek_index.vbri_indexed_bytes) {
      table_relative = s_seek_index.vbri_indexed_bytes;
    }

    uint16_t lo = 0;
    uint16_t hi = count;
    while (lo < hi) {
      const uint16_t mid = (uint16_t)(lo + (hi - lo) / 2u);
      if (cumulative[mid + 1u] < table_relative) lo = (uint16_t)(mid + 1u);
      else hi = mid;
    }
    uint16_t entry = lo < count ? lo : (uint16_t)(count - 1u);
    const uint32_t segment_begin = cumulative[entry];
    const uint32_t segment_end = cumulative[entry + 1u];
    const uint64_t entry_first_frame = (uint64_t)entry * frames_per_entry;
    const uint32_t remaining_frames =
        s_seek_index.vbri_frames > entry_first_frame
            ? (uint32_t)(s_seek_index.vbri_frames - entry_first_frame)
            : 0u;
    const uint32_t entry_frames = remaining_frames < frames_per_entry
        ? remaining_frames
        : (uint32_t)frames_per_entry;
    uint32_t frame_in_entry = 0;
    if (segment_end > segment_begin &&
        table_relative > segment_begin &&
        entry_frames > 0u) {
      frame_in_entry = (uint32_t)(((uint64_t)(table_relative - segment_begin) *
          entry_frames) / (segment_end - segment_begin));
      if (frame_in_entry > entry_frames) frame_in_entry = entry_frames;
    }
    uint64_t frame = entry_first_frame + frame_in_entry;
    if (frame > s_seek_index.vbri_frames) frame = s_seek_index.vbri_frames;
    const uint64_t ms =
        (uint64_t)total_ms * frame / s_seek_index.vbri_frames;
    return ms > total_ms ? total_ms : (uint32_t)ms;
  }

  if (strategy == Mp3SeekStrategy::XingToc && s_seek_index.toc_valid) {
    const uint64_t value_milli =
        (uint64_t)relative * 256000ull / (uint64_t)stream_bytes;
    for (uint32_t i = 0; i < 100u; ++i) {
      const uint32_t a = (uint32_t)s_seek_index.toc[i] * 1000u;
      const uint32_t b = (i < 99u ? (uint32_t)s_seek_index.toc[i + 1u] : 256u) * 1000u;
      if (value_milli > b && i < 99u) continue;

      uint32_t fraction = 0;
      if (b > a && value_milli > a) {
        fraction = (uint32_t)(((value_milli - a) * 1000ull) / (uint64_t)(b - a));
        if (fraction > 1000u) fraction = 1000u;
      }
      const uint64_t percent_milli = (uint64_t)i * 1000ull + fraction;
      const uint64_t ms = (uint64_t)total_ms * percent_milli / 100000ull;
      return ms > total_ms ? total_ms : (uint32_t)ms;
    }
    return total_ms;
  }

  const uint64_t ms = (uint64_t)relative * total_ms / stream_bytes;
  return ms > total_ms ? total_ms : (uint32_t)ms;
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

    const uint32_t frame_offset = current_input_buffer_offset();
    inspect_first_frame_seek_metadata(g_inbuf,
                                      (size_t)info.frame_bytes,
                                      frame_offset,
                                      info);
    observe_frame_bitrate(info.bitrate_kbps > 0 ? (uint32_t)info.bitrate_kbps : 0);
    s_prepared_frame_offset = frame_offset;

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
  reset_seek_index_state();

  // 本地文件已知 ID3v2 结束位置时直接从首个音频字节预填，
  // 避免解码器先扫描整段标签，也确保第一帧可以建立 Xing/Info 索引。
  if (!g_source_is_stream &&
      g_source.audio_data_offset > 0 &&
      g_source.seek &&
      !g_source.seek(g_source.ctx, g_source.audio_data_offset)) {
    set_last_error("id3_skip_seek_failed");
    LOGE("[MP3] 跳过 ID3v2 失败：offset=%lu 名称=%s",
         (unsigned long)g_source.audio_data_offset,
         debug_name ? debug_name : "<null>");
    audio_mp3_stop();
    return false;
  }

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

  // 在返回播放成功前先解析首帧索引。这样即使用户刚起播就拖动，
  // Xing/Info TOC 也已经可用；探测使用独立 minimp3 状态，不消费正式解码缓冲。
  probe_initial_seek_metadata();

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

  s_prepared_frame_offset = 0;
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
  if (out_source_offset) {
    *out_source_offset = s_prepared_frame_offset;
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
  const uint32_t audio_begin =
      s_seek_index.first_frame_found && s_seek_index.first_frame_offset < source_size
          ? s_seek_index.first_frame_offset
          : (g_source.audio_data_offset < source_size ? g_source.audio_data_offset : 0);
  const uint32_t audio_bytes = usable_audio_stream_bytes(source_size, audio_begin);
  if (audio_bytes < 4) {
    set_last_error("seek_source_too_small");
    return false;
  }

  if (target_ms > total_ms) target_ms = total_ms;
  const Mp3SeekStrategy strategy = select_seek_strategy();
  const uint32_t target_offset = calculate_seek_offset(strategy,
                                                       target_ms,
                                                       total_ms,
                                                       source_size,
                                                       audio_begin);

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

  const uint32_t actual_ms = estimate_seek_actual_ms(strategy,
                                                     synced_offset,
                                                     total_ms,
                                                     source_size,
                                                     audio_begin);
  if (out_actual_ms) *out_actual_ms = actual_ms;
  LOGI("[MP3] 跳转成功：策略=%s 目标=%lums 实际=%lums 请求字节=%lu 同步帧=%lu 音频=%lu/%lu 来源=%s",
       seek_strategy_label(strategy),
       (unsigned long)target_ms,
       (unsigned long)actual_ms,
       (unsigned long)target_offset,
       (unsigned long)synced_offset,
       (unsigned long)audio_bytes,
       (unsigned long)source_size,
       g_source_is_stream ? "NAS" : "本地");
  return true;
}

bool audio_mp3_is_active() { return s_mp3_active; }
bool audio_mp3_is_stream_source() { return g_source_is_stream; }
uint32_t audio_mp3_get_sample_rate() { return s_mp3_sample_rate; }
uint8_t audio_mp3_get_channels() { return s_mp3_channels; }
uint32_t audio_mp3_get_bitrate_kbps() { return s_mp3_bitrate_kbps; }
uint32_t audio_mp3_get_repaired_total_ms() { return s_seek_index.repaired_total_ms; }
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
  reset_seek_index_state();
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

  const uint32_t frame_offset = current_input_buffer_offset();
  inspect_first_frame_seek_metadata(g_inbuf,
                                    (size_t)info.frame_bytes,
                                    frame_offset,
                                    info);
  observe_frame_bitrate(info.bitrate_kbps > 0 ? (uint32_t)info.bitrate_kbps : 0);

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
