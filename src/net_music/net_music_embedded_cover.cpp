#include "net_music/net_music_embedded_cover.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <string.h>
#include <stdlib.h>

#include "meta/meta_id3_cover.h"
#include "lyrics/lyrics.h"
#include "player_source.h"
#include "ui/ui.h"
#include "utils/log.h"

namespace {

static constexpr uint32_t kHttpRangeWindowBytes = 16 * 1024u;
static constexpr uint32_t kHttpCoverChunkBytes = 8 * 1024u;
static constexpr uint32_t kMaxRemoteCoverBytes = 768 * 1024u;
static constexpr uint32_t kMaxRemoteLyricsBytes = 64 * 1024u;
static constexpr uint32_t kRemoteLyricsChunkBytes = 512u;
static constexpr uint32_t kLyricsJobDelayMs = 250;
static constexpr uint32_t kCoverJobDelayMs = 600;
static constexpr uint32_t kHttpTimeoutMs = 5000;
static constexpr uint32_t kStartupProbeHttpTimeoutMs = 1500;
static constexpr uint32_t kMaxStartupSkipBytes = 2 * 1024 * 1024u;
static constexpr uint16_t kCoverTaskStackBytes = 12288;
static constexpr UBaseType_t kCoverTaskPrio = 1;

volatile uint32_t s_cover_job_generation = 0;

struct NetCoverRuntimeState {
  bool valid = false;
  int idx = -1;
  String url;
  uint32_t offset = 0;
  uint32_t size = 0;
  String rev;
};

static NetCoverRuntimeState s_runtime_cover;
static StaticSemaphore_t s_runtime_cover_mu_buf;
static SemaphoreHandle_t s_runtime_cover_mu = nullptr;

static SemaphoreHandle_t runtime_cover_mutex()
{
  if (!s_runtime_cover_mu) {
    s_runtime_cover_mu = xSemaphoreCreateMutexStatic(&s_runtime_cover_mu_buf);
  }
  return s_runtime_cover_mu;
}


static uint32_t id3_synchsafe32(const uint8_t* p)
{
  if (!p) return 0;
  return ((uint32_t)(p[0] & 0x7F) << 21) |
         ((uint32_t)(p[1] & 0x7F) << 14) |
         ((uint32_t)(p[2] & 0x7F) << 7) |
         ((uint32_t)(p[3] & 0x7F));
}

static bool http_range_get_small_no_job(const String& url,
                                        uint32_t start,
                                        uint32_t len,
                                        uint8_t* dst,
                                        uint32_t timeout_ms)
{
  if (!dst || len == 0) return false;
  if (!WiFi.isConnected()) return false;

  HTTPClient http;
  http.setTimeout(timeout_ms);
  http.setReuse(false);

  if (!http.begin(url)) {
    return false;
  }

  String range;
  range.reserve(32);
  range += "bytes=";
  range += start;
  range += "-";
  range += (start + len - 1);
  http.addHeader("Range", range);

  const int code = http.GET();
  if (code != HTTP_CODE_PARTIAL_CONTENT) {
    LOGW("[网络歌曲] 起播 Range 探测失败 HTTP=%d range=%s", code, range.c_str());
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  uint32_t copied = 0;
  const uint32_t t0 = millis();

  while (copied < len && http.connected()) {
    const int avail = stream ? stream->available() : 0;
    if (avail > 0) {
      const uint32_t remain = len - copied;
      const uint32_t want = (uint32_t)avail > remain ? remain : (uint32_t)avail;
      const int got = stream->readBytes(dst + copied, want);
      if (got > 0) {
        copied += (uint32_t)got;
        continue;
      }
    }

    if (millis() - t0 > timeout_ms) {
      http.end();
      return false;
    }

    vTaskDelay(1);
  }

  http.end();
  return copied == len;
}

static uint32_t fnv1a_add_bytes(uint32_t h, const char* s)
{
  if (!s) return h;
  const uint8_t* p = reinterpret_cast<const uint8_t*>(s);
  while (*p) {
    h ^= *p++;
    h *= 16777619u;
  }
  return h;
}

static uint32_t fnv1a_add_u32(uint32_t h, uint32_t v)
{
  for (int i = 0; i < 4; ++i) {
    h ^= (uint8_t)((v >> (i * 8)) & 0xFF);
    h *= 16777619u;
  }
  return h;
}

static String make_runtime_cover_rev(int idx, const String& url, uint32_t offset, uint32_t size)
{
  uint32_t h = 2166136261u;
  h = fnv1a_add_u32(h, (uint32_t)idx);
  h = fnv1a_add_u32(h, offset);
  h = fnv1a_add_u32(h, size);
  h = fnv1a_add_bytes(h, url.c_str());
  char buf[16];
  snprintf(buf, sizeof(buf), "%08lx", (unsigned long)h);
  return String(buf);
}

static void clear_runtime_cover_state()
{
  SemaphoreHandle_t mu = runtime_cover_mutex();
  if (!mu) return;
  if (xSemaphoreTake(mu, pdMS_TO_TICKS(30)) != pdTRUE) return;
  s_runtime_cover.valid = false;
  s_runtime_cover.idx = -1;
  s_runtime_cover.url = String();
  s_runtime_cover.offset = 0;
  s_runtime_cover.size = 0;
  s_runtime_cover.rev = String();
  xSemaphoreGive(mu);
}

static void set_runtime_cover_state(int idx, const String& url, uint32_t offset, uint32_t size)
{
  SemaphoreHandle_t mu = runtime_cover_mutex();
  if (!mu) return;
  if (xSemaphoreTake(mu, pdMS_TO_TICKS(30)) != pdTRUE) return;
  s_runtime_cover.valid = true;
  s_runtime_cover.idx = idx;
  s_runtime_cover.url = url;
  s_runtime_cover.offset = offset;
  s_runtime_cover.size = size;
  s_runtime_cover.rev = make_runtime_cover_rev(idx, url, offset, size);
  xSemaphoreGive(mu);
}

static bool is_current_job(uint32_t generation, int idx, const String& url)
{
  if (generation != s_cover_job_generation) {
    return false;
  }

  const PlayerSourceState source = player_source_get();
  return source.type == PlayerSourceType::NET_TRACK &&
         source.net_track_idx == idx &&
         source.net_track_url == url;
}

static bool is_png_buffer(const uint8_t* b, size_t len)
{
  return len >= 8 &&
         b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G' &&
         b[4] == 0x0D && b[5] == 0x0A && b[6] == 0x1A && b[7] == 0x0A;
}

static bool is_jpg_buffer(const uint8_t* b, size_t len)
{
  return len >= 2 && b[0] == 0xFF && b[1] == 0xD8;
}

static String build_same_dir_sidecar_url(const String& media_url, const char* new_ext)
{
  if (!new_ext || !new_ext[0] || media_url.length() == 0) {
    return String();
  }

  String url = media_url;
  String suffix;
  const int q = url.indexOf('?');
  if (q >= 0) {
    suffix = url.substring(q);
    url = url.substring(0, q);
  }

  const int slash = url.lastIndexOf('/');
  const int dot = url.lastIndexOf('.');
  if (dot > slash) {
    url = url.substring(0, dot);
  }

  url += new_ext;
  url += suffix;
  return url;
}

static char* alloc_remote_lyrics_buffer(size_t cap)
{
  char* buf = static_cast<char*>(ps_malloc(cap + 1));
  if (!buf) {
    buf = static_cast<char*>(malloc(cap + 1));
  }
  return buf;
}

static bool http_get_lrc_text(const String& lrc_url,
                              uint32_t generation,
                              int idx,
                              char** out_text,
                              size_t* out_len,
                              int* out_http_code)
{
  if (out_text) *out_text = nullptr;
  if (out_len) *out_len = 0;
  if (out_http_code) *out_http_code = 0;

  if (!out_text || !out_len || lrc_url.length() == 0) return false;
  if (!WiFi.isConnected()) return false;
  if (generation != s_cover_job_generation) return false;

  HTTPClient http;
  http.setTimeout(kHttpTimeoutMs);
  http.setReuse(false);

  if (!http.begin(lrc_url)) {
    return false;
  }

  const int code = http.GET();
  if (out_http_code) *out_http_code = code;
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  const int content_len = http.getSize();
  if (content_len == 0 || content_len > (int)kMaxRemoteLyricsBytes) {
    LOGW("[网络歌词] LRC 大小无效 len=%d url=%s", content_len, lrc_url.c_str());
    http.end();
    return false;
  }

  const size_t cap = content_len > 0 ? (size_t)content_len : (size_t)kMaxRemoteLyricsBytes;
  char* buf = alloc_remote_lyrics_buffer(cap);
  if (!buf) {
    LOGW("[网络歌词] LRC 缓冲分配失败 cap=%u", (unsigned)cap);
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  uint8_t chunk[kRemoteLyricsChunkBytes];
  size_t copied = 0;
  uint32_t last_data_ms = millis();

  while (http.connected()) {
    if (generation != s_cover_job_generation) {
      free(buf);
      http.end();
      return false;
    }

    const int avail = stream ? stream->available() : 0;
    if (avail > 0) {
      const uint32_t want = (uint32_t)avail > kRemoteLyricsChunkBytes
                              ? kRemoteLyricsChunkBytes
                              : (uint32_t)avail;
      const int got = stream->readBytes(chunk, want);
      if (got > 0) {
        if (copied + (size_t)got > kMaxRemoteLyricsBytes) {
          LOGW("[网络歌词] LRC 超过上限 copied=%u got=%d url=%s",
               (unsigned)copied,
               got,
               lrc_url.c_str());
          free(buf);
          http.end();
          return false;
        }
        memcpy(buf + copied, chunk, (size_t)got);
        copied += (size_t)got;
        last_data_ms = millis();

        if (content_len > 0 && copied >= (size_t)content_len) {
          break;
        }
        continue;
      }
    }

    if (content_len > 0 && copied >= (size_t)content_len) {
      break;
    }

    if (millis() - last_data_ms > kHttpTimeoutMs) {
      LOGW("[网络歌词] LRC 下载超时 copied=%u url=%s", (unsigned)copied, lrc_url.c_str());
      free(buf);
      http.end();
      return false;
    }

    vTaskDelay(1);
  }

  http.end();

  if (copied == 0) {
    free(buf);
    return false;
  }

  buf[copied] = '\0';
  *out_text = buf;
  *out_len = copied;
  return true;
}

static bool fetch_and_apply_same_dir_lrc(const String& mp3_url, uint32_t generation, int idx)
{
  const String lrc_urls[] = {
    build_same_dir_sidecar_url(mp3_url, ".lrc"),
    build_same_dir_sidecar_url(mp3_url, ".LRC"),
  };

  for (const String& lrc_url : lrc_urls) {
    if (!lrc_url.length()) continue;
    if (!is_current_job(generation, idx, mp3_url)) return false;

    char* lyrics_text = nullptr;
    size_t lyrics_len = 0;
    int http_code = 0;

    const uint32_t t0 = millis();
    const bool ok = http_get_lrc_text(lrc_url,
                                      generation,
                                      idx,
                                      &lyrics_text,
                                      &lyrics_len,
                                      &http_code);
    if (!ok) {
      if (http_code != 404 && http_code != 0) {
        LOGD("[网络歌词] LRC 下载失败 HTTP=%d url=%s", http_code, lrc_url.c_str());
      }
      continue;
    }

    if (!is_current_job(generation, idx, mp3_url)) {
      free(lyrics_text);
      return false;
    }

    const bool loaded = g_lyricsDisplay.loadFromOwnedTextBuffer(lyrics_text, lyrics_len);
    lyrics_text = nullptr;

    if (loaded && is_current_job(generation, idx, mp3_url)) {
      ui_request_refresh_now();
      LOGI("[网络歌词] 同目录 LRC 已加载 idx=%d bytes=%u 耗时=%lums url=%s",
           idx,
           (unsigned)lyrics_len,
           (unsigned long)(millis() - t0),
           lrc_url.c_str());
      return true;
    }

    LOGW("[网络歌词] LRC 解析失败 idx=%d bytes=%u url=%s",
         idx,
         (unsigned)lyrics_len,
         lrc_url.c_str());
    return false;
  }

  LOGD("[网络歌词] 未找到同目录 LRC idx=%d url=%s", idx, mp3_url.c_str());
  return false;
}

static bool http_range_get_exact(const String& url,
                                 uint32_t start,
                                 uint32_t len,
                                 uint8_t* dst,
                                 uint32_t* out_got,
                                 uint32_t generation,
                                 int idx)
{
  if (out_got) *out_got = 0;
  if (!dst || len == 0) return false;
  if (!WiFi.isConnected()) return false;
  if (generation != s_cover_job_generation) return false;

  HTTPClient http;
  http.setTimeout(kHttpTimeoutMs);
  http.setReuse(false);

  if (!http.begin(url)) {
    return false;
  }

  String range;
  range.reserve(32);
  range += "bytes=";
  range += start;
  range += "-";
  range += (start + len - 1);
  http.addHeader("Range", range);

  const int code = http.GET();
  if (code != HTTP_CODE_PARTIAL_CONTENT) {
    LOGW("[网络封面] Range 请求失败 HTTP=%d range=%s", code, range.c_str());
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  uint32_t copied = 0;
  uint32_t last_data_ms = millis();

  while (copied < len && http.connected()) {
    if (generation != s_cover_job_generation) {
      http.end();
      return false;
    }

    const int avail = stream ? stream->available() : 0;
    if (avail > 0) {
      const uint32_t remain = len - copied;
      const uint32_t want = (uint32_t)avail > remain ? remain : (uint32_t)avail;
      const int got = stream->readBytes(dst + copied, want);
      if (got > 0) {
        copied += (uint32_t)got;
        last_data_ms = millis();
        continue;
      }
    }

    if (millis() - last_data_ms > kHttpTimeoutMs) {
      LOGW("[网络封面] Range 读取超时 copied=%lu/%lu",
           (unsigned long)copied,
           (unsigned long)len);
      http.end();
      return false;
    }

    vTaskDelay(1);
  }

  http.end();

  if (out_got) *out_got = copied;
  return copied == len;
}

class HttpRangeId3Reader final : public Id3ByteReader {
public:
  HttpRangeId3Reader(const String& url, uint32_t generation, int idx)
      : m_url(url), m_generation(generation), m_idx(idx) {}

  ~HttpRangeId3Reader() override {
    if (m_buf) {
      heap_caps_free(m_buf);
      m_buf = nullptr;
    }
  }

  bool read(void* dst, size_t n) override {
    if (!dst) return false;
    uint8_t* out = static_cast<uint8_t*>(dst);
    size_t done = 0;

    while (done < n) {
      if (!ensure_window(m_pos)) {
        return false;
      }

      const uint32_t off = m_pos - m_window_start;
      const uint32_t avail = m_window_len > off ? (m_window_len - off) : 0;
      if (avail == 0) return false;

      const size_t want = ((n - done) < avail) ? (n - done) : avail;
      memcpy(out + done, m_buf + off, want);
      done += want;
      m_pos += (uint32_t)want;
    }

    return true;
  }

  int readByte() override {
    uint8_t b = 0;
    if (!read(&b, 1)) return -1;
    return b;
  }

  bool seek(uint32_t pos) override {
    m_pos = pos;
    return true;
  }

  bool skip(uint32_t n) override {
    m_pos += n;
    return true;
  }

  uint32_t position() const override {
    return m_pos;
  }

private:
  bool ensure_window(uint32_t pos) {
    if (m_buf && pos >= m_window_start && pos < m_window_start + m_window_len) {
      return true;
    }

    if (m_generation != s_cover_job_generation) {
      return false;
    }

    if (!m_buf) {
      m_buf = static_cast<uint8_t*>(heap_caps_malloc(kHttpRangeWindowBytes,
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      if (!m_buf) {
        m_buf = static_cast<uint8_t*>(heap_caps_malloc(kHttpRangeWindowBytes, MALLOC_CAP_8BIT));
      }
      if (!m_buf) {
        LOGW("[网络封面] Range window 分配失败 size=%lu", (unsigned long)kHttpRangeWindowBytes);
        return false;
      }
    }

    uint32_t got = 0;
    if (!http_range_get_exact(m_url,
                              pos,
                              kHttpRangeWindowBytes,
                              m_buf,
                              &got,
                              m_generation,
                              m_idx)) {
      return false;
    }

    m_window_start = pos;
    m_window_len = got;
    return got > 0;
  }

  String m_url;
  uint32_t m_generation = 0;
  int m_idx = -1;
  uint32_t m_pos = 0;
  uint8_t* m_buf = nullptr;
  uint32_t m_window_start = 0;
  uint32_t m_window_len = 0;
};

struct NetCoverJob {
  int idx = -1;
  uint32_t generation = 0;
  String url;
};

static bool fetch_remote_cover_image(const String& url,
                                     const Mp3CoverLoc& loc,
                                     uint32_t generation,
                                     int idx,
                                     uint8_t** out_buf,
                                     size_t* out_len,
                                     bool* out_is_png)
{
  if (out_buf) *out_buf = nullptr;
  if (out_len) *out_len = 0;
  if (out_is_png) *out_is_png = false;

  if (!out_buf || !out_len || !out_is_png) return false;
  if (!loc.found || loc.size == 0 || loc.size > kMaxRemoteCoverBytes) {
    LOGW("[网络封面] APIC size 无效 size=%lu", (unsigned long)loc.size);
    return false;
  }

  uint8_t* buf = static_cast<uint8_t*>(heap_caps_malloc(loc.size,
                                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!buf) {
    buf = static_cast<uint8_t*>(heap_caps_malloc(loc.size, MALLOC_CAP_8BIT));
  }
  if (!buf) {
    LOGW("[网络封面] 图片缓冲分配失败 size=%lu", (unsigned long)loc.size);
    return false;
  }

  uint32_t copied = 0;
  while (copied < loc.size) {
    if (generation != s_cover_job_generation) {
      heap_caps_free(buf);
      return false;
    }

    const uint32_t remain = loc.size - copied;
    const uint32_t chunk = remain > kHttpCoverChunkBytes ? kHttpCoverChunkBytes : remain;
    uint32_t got = 0;

    if (!http_range_get_exact(url,
                              loc.offset + copied,
                              chunk,
                              buf + copied,
                              &got,
                              generation,
                              idx) || got != chunk) {
      heap_caps_free(buf);
      return false;
    }

    copied += got;
    vTaskDelay(1);
  }

  const String mime_lower = loc.mime;
  String mime = mime_lower;
  mime.toLowerCase();

  *out_is_png = mime.indexOf("png") >= 0 || is_png_buffer(buf, loc.size);
  if (!*out_is_png && !is_jpg_buffer(buf, loc.size)) {
    LOGW("[网络封面] 图片头不是 JPG/PNG，mime=%s", loc.mime.c_str());
  }

  *out_buf = buf;
  *out_len = loc.size;
  return true;
}

static void net_cover_task_entry(void* arg)
{
  NetCoverJob* job = static_cast<NetCoverJob*>(arg);
  if (!job) {
    vTaskDelete(nullptr);
    return;
  }

  const int idx = job->idx;
  const uint32_t generation = job->generation;
  const String url = job->url;
  delete job;

  const uint32_t task_start_ms = millis();

  vTaskDelay(pdMS_TO_TICKS(kLyricsJobDelayMs));
  if (!is_current_job(generation, idx, url)) {
    vTaskDelete(nullptr);
    return;
  }

  (void)fetch_and_apply_same_dir_lrc(url, generation, idx);

  const uint32_t elapsed_ms = millis() - task_start_ms;
  if (elapsed_ms < kCoverJobDelayMs) {
    vTaskDelay(pdMS_TO_TICKS(kCoverJobDelayMs - elapsed_ms));
  }

  if (!is_current_job(generation, idx, url)) {
    vTaskDelete(nullptr);
    return;
  }

  const uint32_t t0 = millis();
  Mp3CoverLoc loc;
  HttpRangeId3Reader reader(url, generation, idx);

  if (!id3_find_apic_from_reader(reader, loc) || !loc.found) {
    LOGD("[网络封面] 未找到 NAS MP3 内嵌 APIC idx=%d", idx);
    vTaskDelete(nullptr);
    return;
  }

  if (!is_current_job(generation, idx, url)) {
    vTaskDelete(nullptr);
    return;
  }

  uint8_t* cover_buf = nullptr;
  size_t cover_len = 0;
  bool cover_is_png = false;

  if (!fetch_remote_cover_image(url,
                                loc,
                                generation,
                                idx,
                                &cover_buf,
                                &cover_len,
                                &cover_is_png)) {
    LOGW("[网络封面] 下载 APIC 图片失败 idx=%d off=%lu size=%lu",
         idx,
         (unsigned long)loc.offset,
         (unsigned long)loc.size);
    vTaskDelete(nullptr);
    return;
  }

  if (!is_current_job(generation, idx, url)) {
    heap_caps_free(cover_buf);
    vTaskDelete(nullptr);
    return;
  }

  const bool scaled_ok = ui_cover_scale_from_buffer(cover_buf, cover_len, cover_is_png);
  heap_caps_free(cover_buf);

  if (scaled_ok && is_current_job(generation, idx, url)) {
    const bool web_ok = ui_cover_store_current_web_cache(idx,
                                                         COVER_MP3_APIC,
                                                         url.c_str(),
                                                         "",
                                                         loc.offset,
                                                         loc.size);
    if (web_ok) {
      set_runtime_cover_state(idx, url, loc.offset, loc.size);
    } else {
      LOGW("[网络封面] 网页封面缓存写入失败 idx=%d", idx);
    }
    ui_request_refresh_now();
    LOGI("[网络封面] NAS 内嵌封面已应用 idx=%d size=%u png=%u web=%u 耗时=%lums",
         idx,
         (unsigned)cover_len,
         cover_is_png ? 1 : 0,
         web_ok ? 1 : 0,
         (unsigned long)(millis() - t0));
  } else {
    LOGW("[网络封面] NAS 内嵌封面缩放失败 idx=%d size=%u", idx, (unsigned)cover_len);
  }

  vTaskDelete(nullptr);
}

}  // namespace

bool net_music_mp3_probe_audio_start_offset(const String& mp3_url, uint32_t* out_offset)
{
  if (out_offset) *out_offset = 0;
  if (!out_offset || mp3_url.length() == 0) return false;

  uint8_t hdr[10] = {0};
  const uint32_t t0 = millis();

  if (!http_range_get_small_no_job(mp3_url, 0, sizeof(hdr), hdr, kStartupProbeHttpTimeoutMs)) {
    LOGW("[网络歌曲] 起播 ID3 探测失败，回退普通起播 URL=%s", mp3_url.c_str());
    return false;
  }

  if (hdr[0] != 'I' || hdr[1] != 'D' || hdr[2] != '3') {
    *out_offset = 0;
    LOGD("[网络歌曲] 起播 ID3 探测：无 ID3v2 耗时=%lums",
         (unsigned long)(millis() - t0));
    return true;
  }

  const uint8_t major = hdr[3];
  const uint8_t flags = hdr[5];
  uint32_t tag_size = id3_synchsafe32(hdr + 6);
  uint32_t offset = 10u + tag_size;

  // ID3v2.4 footer flag。多数 MP3 没有 footer；有 footer 时音频帧在 footer 之后。
  if (major >= 4 && (flags & 0x10)) {
    offset += 10u;
  }

  if (offset <= 10u || offset > kMaxStartupSkipBytes) {
    LOGW("[网络歌曲] 起播 ID3 offset 异常 version=2.%u tag=%lu offset=%lu，回退普通起播",
         (unsigned)major,
         (unsigned long)tag_size,
         (unsigned long)offset);
    *out_offset = 0;
    return false;
  }

  *out_offset = offset;
  LOGI("[网络歌曲] 起播跳过 ID3v2.%u tag=%lu offset=%lu 耗时=%lums",
       (unsigned)major,
       (unsigned long)tag_size,
       (unsigned long)offset,
       (unsigned long)(millis() - t0));
  return true;
}

void net_music_embedded_cover_start(int net_track_idx, const String& mp3_url)
{
  if (net_track_idx < 0 || !mp3_url.length()) {
    return;
  }

  const uint32_t generation = ++s_cover_job_generation;
  clear_runtime_cover_state();

  NetCoverJob* job = new NetCoverJob();
  if (!job) {
    LOGW("[网络封面] job 分配失败");
    return;
  }

  job->idx = net_track_idx;
  job->generation = generation;
  job->url = mp3_url;

  BaseType_t ok = xTaskCreatePinnedToCore(net_cover_task_entry,
                                          "NetMp3Cover",
                                          kCoverTaskStackBytes,
                                          job,
                                          kCoverTaskPrio,
                                          nullptr,
                                          1);
  if (ok != pdPASS) {
    LOGW("[网络封面] 创建任务失败 idx=%d", net_track_idx);
    delete job;
  }
}

void net_music_embedded_cover_cancel()
{
  ++s_cover_job_generation;
  clear_runtime_cover_state();
}

bool net_music_embedded_cover_get_current(int net_track_idx,
                                          const String& mp3_url,
                                          uint32_t* out_offset,
                                          uint32_t* out_size,
                                          String* out_rev)
{
  SemaphoreHandle_t mu = runtime_cover_mutex();
  if (!mu) return false;
  if (xSemaphoreTake(mu, pdMS_TO_TICKS(20)) != pdTRUE) return false;

  const bool ok = s_runtime_cover.valid &&
                  s_runtime_cover.idx == net_track_idx &&
                  s_runtime_cover.url == mp3_url &&
                  s_runtime_cover.size > 0;
  if (ok) {
    if (out_offset) *out_offset = s_runtime_cover.offset;
    if (out_size) *out_size = s_runtime_cover.size;
    if (out_rev) *out_rev = s_runtime_cover.rev;
  }

  xSemaphoreGive(mu);
  return ok;
}
