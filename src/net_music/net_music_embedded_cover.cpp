#include "net_music/net_music_embedded_cover.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
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
static constexpr uint32_t kMaxRemoteCoverBytes = 512 * 1024u;
static constexpr uint32_t kMaxRemoteLyricsBytes = 64 * 1024u;
static constexpr uint32_t kRemoteLyricsChunkBytes = 512u;
static constexpr size_t kInternalFallbackMaxBytes = 32 * 1024;
static constexpr uint32_t kLyricsJobDelayMs = 250;
static constexpr uint32_t kCoverJobDelayMs = 600;
static constexpr uint32_t kHttpTimeoutMs = 5000;
static constexpr uint16_t kCoverTaskStackBytes = 12288;
static constexpr UBaseType_t kCoverTaskPrio = 1;
static constexpr UBaseType_t kCoverTaskCore = 1;
static constexpr UBaseType_t kCoverQueueLength = 1;

static portMUX_TYPE s_cover_generation_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_cover_job_generation = 0;
static QueueHandle_t s_cover_job_queue = nullptr;
static TaskHandle_t s_cover_task_handle = nullptr;
static StaticSemaphore_t s_cover_worker_mu_buf;
static SemaphoreHandle_t s_cover_worker_mu = nullptr;

static SemaphoreHandle_t cover_worker_mutex()
{
  if (!s_cover_worker_mu) {
    s_cover_worker_mu = xSemaphoreCreateMutexStatic(&s_cover_worker_mu_buf);
  }
  return s_cover_worker_mu;
}

static uint32_t current_cover_generation()
{
  portENTER_CRITICAL(&s_cover_generation_mux);
  const uint32_t generation = s_cover_job_generation;
  portEXIT_CRITICAL(&s_cover_generation_mux);
  return generation;
}

static uint32_t advance_cover_generation()
{
  portENTER_CRITICAL(&s_cover_generation_mux);
  ++s_cover_job_generation;
  if (s_cover_job_generation == 0) {
    s_cover_job_generation = 1;
  }
  const uint32_t generation = s_cover_job_generation;
  portEXIT_CRITICAL(&s_cover_generation_mux);
  return generation;
}

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
  if (generation != current_cover_generation()) {
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
  const size_t bytes = cap + 1;
  char* buf = static_cast<char*>(ps_malloc(bytes));
  if (!buf && bytes <= kInternalFallbackMaxBytes) {
    buf = static_cast<char*>(malloc(bytes));
  }
  if (!buf && bytes > kInternalFallbackMaxBytes) {
    LOGW("[网络歌词] 缓冲 PSRAM 分配失败，禁止回落内部RAM size=%lu",
         (unsigned long)bytes);
  }
  return buf;
}

static bool http_get_lrc_text(const String& lrc_url,
                              uint32_t generation,
                              char** out_text,
                              size_t* out_len,
                              int* out_http_code)
{
  if (out_text) *out_text = nullptr;
  if (out_len) *out_len = 0;
  if (out_http_code) *out_http_code = 0;

  if (!out_text || !out_len || lrc_url.length() == 0) return false;
  if (!WiFi.isConnected()) return false;
  if (generation != current_cover_generation()) return false;

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
    if (generation != current_cover_generation()) {
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
                                 uint32_t generation)
{
  if (out_got) *out_got = 0;
  if (!dst || len == 0) return false;
  if (!WiFi.isConnected()) return false;
  if (generation != current_cover_generation()) return false;

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
    if (generation != current_cover_generation()) {
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
  HttpRangeId3Reader(const String& url, uint32_t generation)
      : m_url(url), m_generation(generation) {}

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

    if (m_generation != current_cover_generation()) {
      return false;
    }

    if (!m_buf) {
      m_buf = static_cast<uint8_t*>(heap_caps_malloc(kHttpRangeWindowBytes,
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      if (!m_buf && kHttpRangeWindowBytes <= kInternalFallbackMaxBytes) {
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
                              m_generation)) {
      return false;
    }

    m_window_start = pos;
    m_window_len = got;
    return got > 0;
  }

  String m_url;
  uint32_t m_generation = 0;
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
  if (!buf && loc.size <= kInternalFallbackMaxBytes) {
    buf = static_cast<uint8_t*>(heap_caps_malloc(loc.size, MALLOC_CAP_8BIT));
  }
  if (!buf) {
    if (loc.size > kInternalFallbackMaxBytes) {
      LOGW("[网络封面] 图片缓冲 PSRAM 分配失败，禁止回落内部RAM size=%lu", (unsigned long)loc.size);
    } else {
      LOGW("[网络封面] 图片缓冲分配失败 size=%lu", (unsigned long)loc.size);
    }
    return false;
  }

  uint32_t copied = 0;
  while (copied < loc.size) {
    if (generation != current_cover_generation()) {
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
                              generation) || got != chunk) {
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

static void process_net_cover_job(int idx, uint32_t generation, const String& url)
{
  const uint32_t task_start_ms = millis();

  vTaskDelay(pdMS_TO_TICKS(kLyricsJobDelayMs));
  if (!is_current_job(generation, idx, url)) {
    return;
  }

  (void)fetch_and_apply_same_dir_lrc(url, generation, idx);

  const uint32_t elapsed_ms = millis() - task_start_ms;
  if (elapsed_ms < kCoverJobDelayMs) {
    vTaskDelay(pdMS_TO_TICKS(kCoverJobDelayMs - elapsed_ms));
  }

  if (!is_current_job(generation, idx, url)) {
    return;
  }

  const uint32_t t0 = millis();
  Mp3CoverLoc loc;
  HttpRangeId3Reader reader(url, generation);

  if (!id3_find_apic_from_reader(reader, loc) || !loc.found) {
    LOGD("[网络封面] 未找到 NAS MP3 内嵌 APIC idx=%d", idx);
    return;
  }

  if (!is_current_job(generation, idx, url)) {
    return;
  }

  uint8_t* cover_buf = nullptr;
  size_t cover_len = 0;
  bool cover_is_png = false;

  if (!fetch_remote_cover_image(url,
                                loc,
                                generation,
                                &cover_buf,
                                &cover_len,
                                &cover_is_png)) {
    if (is_current_job(generation, idx, url)) {
      LOGW("[网络封面] 下载 APIC 图片失败 idx=%d off=%lu size=%lu",
           idx,
           (unsigned long)loc.offset,
           (unsigned long)loc.size);
    }
    return;
  }

  if (!is_current_job(generation, idx, url)) {
    heap_caps_free(cover_buf);
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
  } else if (is_current_job(generation, idx, url)) {
    LOGW("[网络封面] NAS 内嵌封面缩放失败 idx=%d size=%u", idx, (unsigned)cover_len);
  }
}

static void net_cover_task_entry(void*)
{
  LOGI("[网络封面] 常驻任务已启动：队列=%u 栈=%u 优先级=%u 核心=%u",
       (unsigned)kCoverQueueLength,
       (unsigned)kCoverTaskStackBytes,
       (unsigned)kCoverTaskPrio,
       (unsigned)kCoverTaskCore);

  for (;;) {
    NetCoverJob* job = nullptr;
    if (!s_cover_job_queue ||
        xQueueReceive(s_cover_job_queue, &job, portMAX_DELAY) != pdTRUE) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    if (!job) {
      continue;
    }

    const int idx = job->idx;
    const uint32_t generation = job->generation;
    const String url = job->url;
    delete job;

    process_net_cover_job(idx, generation, url);
  }
}

static bool ensure_net_cover_worker_started()
{
  SemaphoreHandle_t mu = cover_worker_mutex();
  if (!mu || xSemaphoreTake(mu, pdMS_TO_TICKS(100)) != pdTRUE) {
    LOGW("[网络封面] 获取常驻任务初始化锁失败");
    return false;
  }

  if (s_cover_job_queue && s_cover_task_handle) {
    xSemaphoreGive(mu);
    return true;
  }

  if (!s_cover_job_queue) {
    s_cover_job_queue = xQueueCreate(kCoverQueueLength, sizeof(NetCoverJob*));
    if (!s_cover_job_queue) {
      LOGW("[网络封面] 创建覆盖队列失败");
      xSemaphoreGive(mu);
      return false;
    }
  }

  if (!s_cover_task_handle) {
    const BaseType_t ok = xTaskCreatePinnedToCore(net_cover_task_entry,
                                                  "NetCoverTask",
                                                  kCoverTaskStackBytes,
                                                  nullptr,
                                                  kCoverTaskPrio,
                                                  &s_cover_task_handle,
                                                  kCoverTaskCore);
    if (ok != pdPASS) {
      LOGW("[网络封面] 创建常驻任务失败");
      vQueueDelete(s_cover_job_queue);
      s_cover_job_queue = nullptr;
      s_cover_task_handle = nullptr;
      xSemaphoreGive(mu);
      return false;
    }
  }

  xSemaphoreGive(mu);
  return true;
}

static void discard_pending_cover_job_locked()
{
  if (!s_cover_job_queue) {
    return;
  }

  NetCoverJob* pending = nullptr;
  if (xQueueReceive(s_cover_job_queue, &pending, 0) == pdTRUE && pending) {
    delete pending;
  }
}

static bool submit_latest_cover_job(NetCoverJob* job)
{
  if (!job) {
    return false;
  }

  SemaphoreHandle_t mu = cover_worker_mutex();
  if (!mu || xSemaphoreTake(mu, pdMS_TO_TICKS(100)) != pdTRUE) {
    return false;
  }

  discard_pending_cover_job_locked();
  const bool queued = s_cover_job_queue &&
                      xQueueSend(s_cover_job_queue, &job, 0) == pdTRUE;
  xSemaphoreGive(mu);
  return queued;
}

static void discard_pending_cover_job()
{
  SemaphoreHandle_t mu = cover_worker_mutex();
  if (!mu || xSemaphoreTake(mu, pdMS_TO_TICKS(100)) != pdTRUE) {
    return;
  }

  discard_pending_cover_job_locked();
  xSemaphoreGive(mu);
}

}  // namespace

void net_music_embedded_cover_start(int net_track_idx, const String& mp3_url)
{
  if (net_track_idx < 0 || !mp3_url.length()) {
    return;
  }

  const uint32_t generation = advance_cover_generation();
  clear_runtime_cover_state();

  if (!ensure_net_cover_worker_started()) {
    return;
  }

  NetCoverJob* job = new NetCoverJob();
  if (!job) {
    LOGW("[网络封面] job 分配失败 idx=%d", net_track_idx);
    return;
  }

  job->idx = net_track_idx;
  job->generation = generation;
  job->url = mp3_url;

  // 队列长度固定为1：新请求先清理尚未开始的旧请求，只保留最新曲目。
  // 正在执行的旧请求通过 generation 在下一检查点主动退出，不会再创建额外任务。
  if (!submit_latest_cover_job(job)) {
    LOGW("[网络封面] 提交最新请求失败 idx=%d", net_track_idx);
    delete job;
    return;
  }

  LOGD("[网络封面] 最新请求已提交 idx=%d generation=%lu",
       net_track_idx,
       (unsigned long)generation);
}

void net_music_embedded_cover_cancel()
{
  advance_cover_generation();
  discard_pending_cover_job();
  clear_runtime_cover_state();
  LOGD("[网络封面] 已取消当前与待处理请求 generation=%lu",
       (unsigned long)current_cover_generation());
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
