#include <Arduino.h>
#include <SdFat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "board/board_pins.h"
#include "storage/storage.h"      // 你如果能拿到 SdFat sd 也行
#include "audio/audio.h"
#include "audio/audio_i2s.h"
#include "audio/audio_mp3.h"
#include "audio/audio_flac.h"
#include "audio/audio_file.h"
#include "audio/audio_service.h"
#include "storage/storage_io.h"
#include "dr_flac.h"
#include "utils/log.h"

extern SdFat sd;   // 你工程已有全局 sd

static enum { DEC_NONE, DEC_MP3, DEC_FLAC } g_dec = DEC_NONE;

#ifndef AUDIO_SYNC_SNIFF_ON_PLAY
#define AUDIO_SYNC_SNIFF_ON_PLAY 0
#endif
// --- Total duration (ms), 0 = unknown ---
static volatile uint32_t s_total_ms = 0;
uint32_t audio_get_total_ms() { return s_total_ms; }
void audio_set_total_ms(uint32_t ms) { s_total_ms = ms; }

static bool ends_ci(const char* s, const char* ext)
{
  size_t ls = strlen(s), le = strlen(ext);
  if (ls < le) return false;
  s += (ls - le);
  for (size_t i = 0; i < le; ++i) {
    char a = s[i], b = ext[i];
    if (a >= 'A' && a <= 'Z') a += 32;
    if (b >= 'A' && b <= 'Z') b += 32;
    if (a != b) return false;
  }
  return true;
}

// ===================== Duration sniff (fast, metadata-only) =====================

static inline uint32_t be32(const uint8_t* p)
{
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint32_t id3v2_skip_bytes(AudioFile& f)
{
  uint8_t h[10];
  if (f.read(h, 10) != 10) { f.seek(0); return 0; }
  if (h[0] != 'I' || h[1] != 'D' || h[2] != '3') { f.seek(0); return 0; }

  // synchsafe size (7 bits each)
  uint32_t sz = ((uint32_t)(h[6] & 0x7F) << 21) |
                ((uint32_t)(h[7] & 0x7F) << 14) |
                ((uint32_t)(h[8] & 0x7F) << 7)  |
                ((uint32_t)(h[9] & 0x7F));
  return 10 + sz;
}

static bool mp3_header_ok(uint32_t h)
{
  if ((h & 0xFFE00000u) != 0xFFE00000u) return false;          // sync
  uint32_t ver = (h >> 19) & 3u; if (ver == 1u) return false;   // reserved
  uint32_t lay = (h >> 17) & 3u; if (lay == 0u) return false;   // reserved
  uint32_t br  = (h >> 12) & 0xFu; if (br == 0u || br == 0xFu) return false;
  uint32_t sr  = (h >> 10) & 3u;  if (sr == 3u) return false;
  return true;
}

static uint32_t mp3_sample_rate(uint32_t ver, uint32_t sr_idx)
{
  static const uint16_t sr_mpeg1[3]  = {44100, 48000, 32000};
  static const uint16_t sr_mpeg2[3]  = {22050, 24000, 16000};
  static const uint16_t sr_mpeg25[3] = {11025, 12000,  8000};

  if (sr_idx >= 3) return 0;
  if (ver == 3) return sr_mpeg1[sr_idx];   // MPEG1
  if (ver == 2) return sr_mpeg2[sr_idx];   // MPEG2
  if (ver == 0) return sr_mpeg25[sr_idx];  // MPEG2.5
  return 0;
}

static uint32_t mp3_bitrate_kbps(uint32_t ver, uint32_t layer, uint32_t br_idx)
{
  // layer bits: 01=Layer III, 10=Layer II, 11=Layer I
  if (br_idx >= 16) return 0;
  if (layer != 1) return 0; // 非 Layer III：先不算，返回 0 表示未知

  // MPEG1 Layer III
  static const uint16_t br_mpeg1_l3[16] = {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0};
  // MPEG2/2.5 Layer III
  static const uint16_t br_mpeg2_l3[16] = {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0};

  if (ver == 3) return br_mpeg1_l3[br_idx];
  if (ver == 2 || ver == 0) return br_mpeg2_l3[br_idx];
  return 0;
}

static uint32_t sniff_mp3_total_ms(SdFat& sd, const char* path)
{
  (void)sd;
  uint32_t ms = 0;
  AudioFile f;
  bool buf_mutex_taken = false;
  uint32_t fileSize = 0;
  uint32_t tailCut = 0;
  uint32_t skip = 0;
  uint32_t base = 0;
  uint32_t framePos = 0;
  uint32_t hdr = 0;
  bool found = false;
  uint32_t ver = 0;
  uint32_t layer = 0;
  uint32_t prot = 0;
  uint32_t brIdx = 0;
  uint32_t srIdx = 0;
  uint32_t chMode = 0;
  uint32_t sr = 0;
  uint32_t br_kbps = 0;
  bool mono = false;
  uint32_t sideinfo = 0;
  uint32_t xingOff = 0;
  int hn = 0;
  uint32_t audioBytes = 0;
  uint64_t bits = 0;
  uint32_t frames = 0;
  uint32_t spf = 0;
  uint64_t totalSamples = 0;
  uint32_t flags = 0;
  const uint8_t* p = nullptr;
  const uint8_t* q = nullptr;

  static SemaphoreHandle_t s_buf_mutex = nullptr;
  if (s_buf_mutex == nullptr) {
    s_buf_mutex = xSemaphoreCreateMutex();
    if (s_buf_mutex == nullptr) return 0;
  }

  if (xSemaphoreTake(s_buf_mutex, pdMS_TO_TICKS(500)) != pdTRUE) goto cleanup;
  buf_mutex_taken = true;

  if (!f.open(sd, path)) goto cleanup;

  fileSize = f.size();
  if (fileSize < 16) goto cleanup;

  if (fileSize >= 128) {
    uint8_t tail[3];
    f.seek(fileSize - 128);
    if (f.read(tail, 3) == 3 && tail[0] == 'T' && tail[1] == 'A' && tail[2] == 'G') tailCut = 128;
  }

  f.seek(0);
  skip = id3v2_skip_bytes(f);
  if (skip > 0 && skip < fileSize) f.seek(skip);
  else f.seek(0);

  static uint8_t buf[4096];
  base = f.tell();
  framePos = 0;
  hdr = 0;
  found = false;

  for (int blk = 0; blk < 64; ++blk) {
    uint32_t pos = base + blk * (uint32_t)sizeof(buf);
    f.seek(pos);
    int n = f.read(buf, sizeof(buf));
    if (n < 4) break;

    for (int i = 0; i <= n - 4; ++i) {
      uint32_t h = ((uint32_t)buf[i] << 24) | ((uint32_t)buf[i+1] << 16) | ((uint32_t)buf[i+2] << 8) | (uint32_t)buf[i+3];
      if (!mp3_header_ok(h)) continue;
      framePos = pos + (uint32_t)i;
      hdr = h;
      found = true;
      break;
    }
    if (found) break;
  }

  if (!found) goto cleanup;

  ver   = (hdr >> 19) & 3u;
  layer = (hdr >> 17) & 3u;
  prot  = (hdr >> 16) & 1u;
  brIdx = (hdr >> 12) & 0xFu;
  srIdx = (hdr >> 10) & 3u;
  chMode= (hdr >> 6)  & 3u;

  sr = mp3_sample_rate(ver, srIdx);
  br_kbps = mp3_bitrate_kbps(ver, layer, brIdx);

  mono = (chMode == 3);
  sideinfo = 0;
  if (ver == 3) sideinfo = mono ? 17 : 32;
  else sideinfo = mono ? 9 : 17;

  xingOff = 4 + (prot ? 0 : 2) + sideinfo;

  f.seek(framePos);
  static uint8_t head[256];
  hn = f.read(head, sizeof(head));
  if (hn > (int)(xingOff + 16)) {
    p = head + xingOff;
    if ((p[0]=='X'&&p[1]=='i'&&p[2]=='n'&&p[3]=='g') || (p[0]=='I'&&p[1]=='n'&&p[2]=='f'&&p[3]=='o')) {
      flags = be32(p + 4);
      q = p + 8;
      if (flags & 0x0001u) {
        frames = be32(q);
        spf = (ver == 3) ? 1152u : 576u;
        if (sr > 0 && frames > 0) {
          totalSamples = (uint64_t)frames * (uint64_t)spf;
          ms = (uint32_t)((totalSamples * 1000ull) / (uint64_t)sr);
        }
      }
    }
  }

  if (ms == 0 && br_kbps > 0) {
    audioBytes = fileSize - framePos;
    if (tailCut && audioBytes > tailCut) audioBytes -= tailCut;

    bits = (uint64_t)audioBytes * 8ull;
    ms = (uint32_t)(bits / (uint64_t)br_kbps);
  }

cleanup:
  f.close();
  if (buf_mutex_taken) xSemaphoreGive(s_buf_mutex);
  return ms;
}

// dr_flac callbacks for AudioFile

static size_t flac_on_read(void* pUserData, void* pBufferOut, size_t bytesToRead)
{
  AudioFile* f = (AudioFile*)pUserData;
  if (!f) return 0;
  int n = f->read(pBufferOut, bytesToRead);
  return (n > 0) ? (size_t)n : 0;
}

static drflac_bool32 flac_on_seek(void* pUserData, int offset, drflac_seek_origin origin)
{
  AudioFile* f = (AudioFile*)pUserData;
  if (!f) return DRFLAC_FALSE;

  int64_t base = 0;
  if (origin == DRFLAC_SEEK_CUR) base = (int64_t)f->tell();
  else if (origin == DRFLAC_SEEK_END) base = (int64_t)f->size();
  int64_t pos  = base + (int64_t)offset;
  if (pos < 0) pos = 0;
  return f->seek((uint32_t)pos) ? DRFLAC_TRUE : DRFLAC_FALSE;
}

static drflac_bool32 flac_on_tell(void* pUserData, drflac_int64* pCursor)
{
  AudioFile* f = (AudioFile*)pUserData;
  if (!f || !pCursor) return DRFLAC_FALSE;
  *pCursor = (drflac_int64)f->tell();
  return DRFLAC_TRUE;
}

static uint32_t sniff_flac_total_ms(SdFat& sd, const char* path)
{
  (void)sd;
  uint32_t ms = 0;
  drflac* p = nullptr;
  AudioFile f;

  if (!f.open(sd, path)) goto cleanup;

  p = drflac_open(flac_on_read, flac_on_seek, flac_on_tell, &f, nullptr);
  if (!p) goto cleanup;

  if (p->sampleRate > 0 && p->totalPCMFrameCount > 0) {
    ms = (uint32_t)((p->totalPCMFrameCount * 1000ull) / (uint64_t)p->sampleRate);
  }

cleanup:
  if (p) drflac_close(p);
  f.close();
  return ms;
}

static uint32_t sniff_total_ms(SdFat& sd, const char* path)
{
  if (!path) return 0;
  if (ends_ci(path, ".flac")) return sniff_flac_total_ms(sd, path);
  if (ends_ci(path, ".mp3"))  return sniff_mp3_total_ms(sd, path);
  return 0;
}

uint32_t audio_probe_total_ms(const char* path)
{
  return sniff_total_ms(sd, path);
}

bool audio_init()
{
  if (!storage_sd_init_mutex()) {
    LOGE("[AUDIO] SD 互斥锁未初始化，请确保先调用 storage_init()");
    return false;
  }
  
  // 先按常用 44100 初始化一次（后续会根据文件采样率更新 clk）
  return audio_i2s_init(PIN_I2S_BCLK, PIN_I2S_LRCK, PIN_I2S_DOUT, 44100);
}

void audio_stop()
{
  if (g_dec == DEC_MP3) audio_mp3_stop();
  if (g_dec == DEC_FLAC) audio_flac_stop();
  g_dec = DEC_NONE;
  s_total_ms = 0;
}

bool audio_play(const char* path)
{
  audio_stop();
  audio_reset_play_pos();
  s_total_ms = 0; // round13: 首播路径不再同步探测总时长，优先尽快出声。

  const uint32_t t0 = millis();
  uint32_t t_after_sniff = t0;
  uint32_t t_after_start = t0;
  bool sniff_ran = false;

  Serial.printf("[AUDIO] play: %s\n", path);

#if AUDIO_SYNC_SNIFF_ON_PLAY
  sniff_ran = true;
  s_total_ms = sniff_total_ms(sd, path);
  t_after_sniff = millis();
  if (s_total_ms) {
    Serial.printf("[AUDIO] total_ms=%u\n", (unsigned)s_total_ms);
    Serial.println();
  }
#else
  t_after_sniff = millis();
  LOGD("[AUDIO] sync sniff skipped on play path");
#endif

  bool ok = false;
  if (ends_ci(path, ".mp3")) {
    ok = audio_mp3_start_file(sd, path);
    if (ok) g_dec = DEC_MP3;
  } else if (ends_ci(path, ".flac")) {
    ok = audio_flac_start(sd, path);
    if (ok) g_dec = DEC_FLAC;
  } else {
    Serial.println("[AUDIO] unsupported format");
    return false;
  }

  t_after_start = millis();
  LOGD("[AUDIO] play timing sniff=%lums decoder_start=%lums total=%lums",
       (unsigned long)(t_after_sniff - t0),
       (unsigned long)(t_after_start - t_after_sniff),
       (unsigned long)(t_after_start - t0));
  LOGD("[AUDIO] play detail sniff_ran=%d decoder_stage=%lums",
       sniff_ran ? 1 : 0,
       (unsigned long)(t_after_start - t_after_sniff));

  return ok;
}


bool audio_play_stream_mp3(const char* url)
{
  audio_stop();
  audio_reset_play_pos();
  s_total_ms = 0;
  if (!url) return false;
  Serial.printf("[AUDIO] play stream mp3: %s\n", url);
  bool ok = audio_mp3_start_url(url);
  if (ok) g_dec = DEC_MP3;
  return ok;
}

void audio_loop()
{
  if (g_dec == DEC_MP3) { 
    if (!audio_mp3_loop()) g_dec = DEC_NONE; 
    return; 
  }
  if (g_dec == DEC_FLAC) { 
    if (!audio_flac_loop()) g_dec = DEC_NONE; 
    return; 
  }
}

bool audio_is_playing() { return g_dec != DEC_NONE; }

// ===================== 软件音量（0~100%） =====================
static volatile uint8_t  s_vol_percent = 80;     // 默认 80%
static volatile uint16_t s_gain_q15    = 26214;  // 80% * 32768 / 100 ≈ 26214

void audio_set_volume(uint8_t percent)
{
  if (percent > 100) percent = 100;
  s_vol_percent = percent;
  s_gain_q15 = (uint16_t)((uint32_t)percent * 32768u / 100u); // 0..32768
}

uint8_t audio_get_volume(void)
{
  return s_vol_percent;
}

uint16_t audio_get_gain_q15(void)
{
  // 应用淡入淡出增益
  float fade_gain = audio_service_get_fade_gain();
  uint32_t gain = (uint32_t)(s_gain_q15 * fade_gain);
  if (gain > 32768) gain = 32768;
  return (uint16_t)gain;
}

// 新增函数实现
uint32_t audio_get_play_ms() { return audio_i2s_get_play_ms(); }
void audio_reset_play_pos()  { audio_i2s_reset_play_pos(); }