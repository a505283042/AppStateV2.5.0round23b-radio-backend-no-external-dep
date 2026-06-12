#include <Arduino.h>
#include "audio/audio_flac.h"
#include "audio/audio_i2s.h"
#include "audio/audio_file.h"
#include "board/board_pins.h"
#include "utils/log.h"

#define DR_FLAC_IMPLEMENTATION
#include "../../lib/dr_libs/dr_flac.h"

static AudioFile g_file;
static drflac* g_flac = nullptr;
static bool g_playing = false;
static int g_sr = 44100;
static uint32_t g_ch = 2;
static size_t s_pending_off = 0;
static size_t s_pending_frames = 0;
static const int16_t* s_pending_pcm = nullptr;
static int s_last_sr = 0; // 上次设置的采样率（文件级 static，便于重置）

// FLAC 每次解码 1024 frames，44.1k 下约 23ms。
static constexpr uint32_t FLAC_BUFFER_FRAMES = 1024;
static int16_t s_decode_pcm[FLAC_BUFFER_FRAMES * 2 + 64]; // stereo buffer + 安全边距

// 开播前软件 PCM 缓冲：只预解码，不写 I2S。
// 之前直接预填 I2S DMA 时，I2S 硬件会在功放静音期间把开头音频播放掉，
// 所以无法形成“开声后的缓冲余量”。这里改为先把 PCM 放在 RAM，开声后再快速写入 I2S。
static constexpr uint8_t FLAC_PRIME_CHUNKS = 8;
static int16_t s_prime_pcm[FLAC_PRIME_CHUNKS][FLAC_BUFFER_FRAMES * 2 + 64];
static uint16_t s_prime_frames[FLAC_PRIME_CHUNKS] = {0};
static uint8_t s_prime_head = 0;
static uint8_t s_prime_count = 0;

static void clear_prime_buffer()
{
  s_prime_head = 0;
  s_prime_count = 0;
  for (uint8_t i = 0; i < FLAC_PRIME_CHUNKS; ++i) {
    s_prime_frames[i] = 0;
  }
}

static size_t on_read(void* user, void* bufferOut, size_t bytesToRead)
{
  AudioFile* af = (AudioFile*)user;
  int n = af->read(bufferOut, bytesToRead);
  if (n <= 0) return 0;
  return (size_t)n;
}

static drflac_bool32 on_seek(void* user, int offset, drflac_seek_origin origin)
{
  AudioFile* af = (AudioFile*)user;
  // offset 是 int（可为负），不能直接转成 uint32_t；否则会发生溢出，seek 飞出文件范围。
  const int64_t cur  = (int64_t)af->tell();
  const int64_t size = (int64_t)af->size();
  int64_t base = 0;

  switch (origin) {
    case DRFLAC_SEEK_SET: base = 0;    break;
    case DRFLAC_SEEK_CUR: base = cur;  break;
    case DRFLAC_SEEK_END: base = size; break;
    default: return DRFLAC_FALSE;
  }

  int64_t target = base + (int64_t)offset;
  if (target < 0) target = 0;
  if (target > size) target = size;

  return af->seek((uint32_t)target) ? DRFLAC_TRUE : DRFLAC_FALSE;
}

static drflac_bool32 on_tell(void* user, drflac_int64* pCursor)
{
  AudioFile* af = (AudioFile*)user;
  *pCursor = (drflac_int64)af->tell();
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

bool audio_flac_start(SdFat& sd, const char* path)
{
  audio_flac_stop();
  const uint32_t t0 = millis();
  uint32_t t_after_open = t0;
  uint32_t t_after_drflac_open = t0;
  uint32_t t_after_meta = t0;

  if (!g_file.open(sd, path)) {
    LOGE("[FLAC] open failed: %s", path);
    return false;
  }
  t_after_open = millis();

  g_flac = drflac_open(on_read, on_seek, on_tell, &g_file, nullptr);
  if (!g_flac) {
    LOGE("[FLAC] drflac_open failed");
    g_file.close();
    return false;
  }
  t_after_drflac_open = millis();

  g_sr = (int)g_flac->sampleRate;
  g_ch = g_flac->channels;
  if (g_ch > 2 || g_ch == 0) {
    LOGE("[FLAC] Unsupported channels: %d", g_ch);
    audio_flac_stop();
    return false;
  }
  t_after_meta = millis();
  s_last_sr = 0; // 重置采样率缓存，确保新文件一定会设置 I2S 时钟
  g_playing = true;
  s_pending_pcm = nullptr;
  s_pending_off = 0;
  s_pending_frames = 0;
  clear_prime_buffer();
  const auto& st = g_file.last_open_stats();
  LOGI("[FLAC] start detail lock_wait=%lums dir_prepare=%lums dir_cache=%u cache_reason=%s file_open=%lums file_size=%lums open=%lums drflac_open=%lums meta=%lums total=%lums sr=%d ch=%u",
       (unsigned long)st.lock_wait_ms,
       (unsigned long)st.dir_prepare_ms,
       (unsigned)st.used_dir_cache,
       audio_file_dir_cache_reason_str(st.dir_cache_reason),
       (unsigned long)st.file_open_ms,
       (unsigned long)st.file_size_ms,
       (unsigned long)(t_after_open - t0),
       (unsigned long)(t_after_drflac_open - t_after_open),
       (unsigned long)(t_after_meta - t_after_drflac_open),
       (unsigned long)(t_after_meta - t0),
       g_sr,
       (unsigned)g_ch);
  return true;
}

void audio_flac_stop()
{
  // ✅ 清 pending PCM（非常重要）
  s_pending_off = 0;
  s_pending_frames = 0;
  s_pending_pcm = nullptr;
  clear_prime_buffer();

  if (g_flac) { drflac_close(g_flac); g_flac = nullptr; }
  if (g_file.f) g_file.close();
  g_playing = false;
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

  clear_prime_buffer();

  const uint32_t t0 = millis();
  uint32_t total_frames = 0;
  uint32_t chunks = 0;
  uint32_t max_decode_ms = 0;

  while (chunks < max_chunks) {
    const uint32_t now_ms = (g_sr > 0) ? ((total_frames * 1000UL) / (uint32_t)g_sr) : 0;
    if (now_ms >= target_ms) break;

    const uint32_t idx = (s_prime_head + s_prime_count) % FLAC_PRIME_CHUNKS;
    const uint32_t td0 = millis();
    const uint32_t frames = decode_one_chunk_to(s_prime_pcm[idx]);
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
  LOGI("[FLAC] startup software prime ms=%lu chunks=%lu frames=%lu cost=%lums max_decode=%lums",
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
  if (w == SIZE_MAX) { audio_flac_stop(); return false; }
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

    s_pending_pcm = s_prime_pcm[idx];
    s_pending_off = 0;
    s_pending_frames = s_prime_frames[idx];
    s_prime_frames[idx] = 0;
    return write_pending_pcm();
  }

  // C) 读新 PCM（按 channels 读）
  uint32_t frames_read = decode_one_chunk_to(s_decode_pcm);

  if (frames_read == 0) { audio_flac_stop(); return false; }

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
  if (w == SIZE_MAX) { audio_flac_stop(); return false; }
  s_pending_off += w;
  s_pending_frames -= w;
  if (s_pending_frames == 0) {
    s_pending_off = 0;
    s_pending_pcm = nullptr;
  }

  return true;
}
