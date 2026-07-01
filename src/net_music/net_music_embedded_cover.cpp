#include "net_music/net_music_embedded_cover.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

#include "meta/meta_id3_cover.h"
#include "player_source.h"
#include "ui/ui.h"
#include "utils/log.h"

namespace {

static constexpr uint32_t kHttpRangeWindowBytes = 16 * 1024u;
static constexpr uint32_t kHttpCoverChunkBytes = 8 * 1024u;
static constexpr uint32_t kMaxRemoteCoverBytes = 400 * 1024u;
static constexpr uint32_t kCoverJobDelayMs = 600;
static constexpr uint32_t kHttpTimeoutMs = 5000;
static constexpr uint16_t kCoverTaskStackBytes = 12288;
static constexpr UBaseType_t kCoverTaskPrio = 1;

volatile uint32_t s_cover_job_generation = 0;

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

  vTaskDelay(pdMS_TO_TICKS(kCoverJobDelayMs));

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
    ui_request_refresh_now();
    LOGI("[网络封面] NAS 内嵌封面已应用 idx=%d size=%u png=%u 耗时=%lums",
         idx,
         (unsigned)cover_len,
         cover_is_png ? 1 : 0,
         (unsigned long)(millis() - t0));
  } else {
    LOGW("[网络封面] NAS 内嵌封面缩放失败 idx=%d size=%u", idx, (unsigned)cover_len);
  }

  vTaskDelete(nullptr);
}

}  // namespace

void net_music_embedded_cover_start(int net_track_idx, const String& mp3_url)
{
  if (net_track_idx < 0 || !mp3_url.length()) {
    return;
  }

  const uint32_t generation = ++s_cover_job_generation;

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
}
