// MP3 HTTP stream source adapter
// 职责：
// 1) 使用 WiFiClient 直接打开 HTTP MP3/ICY 网络流
// 2) 解析并跳过 HTTP 响应头，支持 301/302/307/308 一次跳转
// 3) 适配为 AudioMp3Source
// 4) 不负责 MP3 解码

#include "audio/audio_mp3_source_audiotools.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "utils/log.h"

#define LOG_TAG "APP"

namespace {

static WiFiClient g_client;
static String s_url;
static bool s_open = false;
static portMUX_TYPE s_snapshot_mux = portMUX_INITIALIZER_UNLOCKED;
static AudioMp3HttpSourceSnapshot s_snapshot;
static portMUX_TYPE s_operation_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_operation_generation = 1;
static uint32_t s_active_operation_id = 0;

constexpr uint16_t kDefaultHttpPort = 80;
constexpr uint32_t kConnectTimeoutMs = 5000;
constexpr uint32_t kConnectSliceMs = 500;
constexpr uint32_t kConnectRetryDelayMs = 25;
constexpr uint32_t kHeaderTimeoutMs = 6000;
constexpr uint32_t kStartupProbeTimeoutMs = 1500;
constexpr uint32_t kMaxStartupSkipBytes = 2 * 1024 * 1024u;
constexpr int kMaxRedirects = 2;

struct ParsedUrl {
  String host;
  String path;
  uint16_t port = kDefaultHttpPort;
};

static bool operation_is_current(uint32_t operation_id)
{
  if (operation_id == 0) return false;

  bool current = false;
  portENTER_CRITICAL(&s_operation_mux);
  current = (operation_id == s_operation_generation);
  portEXIT_CRITICAL(&s_operation_mux);
  return current;
}

static void publish_source_snapshot(bool open,
                                    bool transport_connected,
                                    bool waiting_for_data,
                                    bool eof,
                                    uint32_t available_bytes,
                                    bool mark_data_activity = false)
{
  const uint32_t now_ms = millis();

  portENTER_CRITICAL(&s_snapshot_mux);
  s_snapshot.open = open;
  s_snapshot.transport_connected = transport_connected;
  s_snapshot.waiting_for_data = waiting_for_data;
  s_snapshot.eof = eof;
  s_snapshot.available_bytes = available_bytes;
  if (!open && !eof) {
    // 主动关闭或新连接开始时清空上一条流的数据时间，避免状态页显示旧值。
    s_snapshot.last_data_ms = 0;
  } else if (mark_data_activity) {
    s_snapshot.last_data_ms = now_ms;
  }
  s_snapshot.last_update_ms = now_ms;
  portEXIT_CRITICAL(&s_snapshot_mux);
}

static bool parse_http_url(const char* url, ParsedUrl& out)
{
  if (!url || !*url) return false;

  String u(url);
  u.trim();

  const char* prefix = "http://";
  if (!u.startsWith(prefix)) {
    LOGE("[电台] only http 流 is 支持ed: %s", url);
    return false;
  }

  String rest = u.substring(strlen(prefix));
  const int slash = rest.indexOf('/');
  String host_port = slash >= 0 ? rest.substring(0, slash) : rest;
  out.path = slash >= 0 ? rest.substring(slash) : String("/");

  if (host_port.length() == 0) return false;

  const int colon = host_port.lastIndexOf(':');
  if (colon > 0) {
    out.host = host_port.substring(0, colon);
    const int port = host_port.substring(colon + 1).toInt();
    out.port = port > 0 ? static_cast<uint16_t>(port) : kDefaultHttpPort;
  } else {
    out.host = host_port;
    out.port = kDefaultHttpPort;
  }

  return out.host.length() > 0 && out.path.length() > 0;
}

static bool read_line_with_timeout(String& line, uint32_t timeout_ms, uint32_t operation_id)
{
  line = "";
  const uint32_t start = millis();

  while (millis() - start < timeout_ms) {
    if (!operation_is_current(operation_id)) {
      LOGD("[电台] HTTP 头读取已取消：操作=%lu", (unsigned long)operation_id);
      return false;
    }

    while (g_client.available() > 0) {
      const char c = static_cast<char>(g_client.read());
      if (c == '\r') continue;
      if (c == '\n') return true;
      line += c;
      if (line.length() > 512) {
        LOGW("[电台] HTTP 头行过长");
        return false;
      }
    }

    if (!g_client.connected()) {
      return line.length() > 0;
    }

    delay(5);
  }

  LOGW("[电台] HTTP 头 读取 超时");
  return false;
}

static uint32_t id3_synchsafe32(const uint8_t* p)
{
  if (!p) return 0;
  return ((uint32_t)(p[0] & 0x7F) << 21) |
         ((uint32_t)(p[1] & 0x7F) << 14) |
         ((uint32_t)(p[2] & 0x7F) << 7) |
         ((uint32_t)(p[3] & 0x7F));
}

static bool read_body_exact(uint8_t* dst,
                            size_t len,
                            uint32_t timeout_ms,
                            uint32_t operation_id)
{
  if (!dst || len == 0) return false;

  size_t copied = 0;
  const uint32_t started_ms = millis();
  while (copied < len && millis() - started_ms < timeout_ms) {
    if (!operation_is_current(operation_id) || !WiFi.isConnected()) {
      return false;
    }

    const int available = g_client.available();
    if (available > 0) {
      size_t want = len - copied;
      if ((size_t)available < want) want = (size_t)available;
      const int n = g_client.read(dst + copied, want);
      if (n > 0) {
        copied += (size_t)n;
        continue;
      }
    }

    if (!g_client.connected() && g_client.available() <= 0) {
      break;
    }

    vTaskDelay(pdMS_TO_TICKS(5));
  }

  return copied == len;
}

static int parse_status_code(const String& status_line)
{
  // 支持 HTTP/1.x 200，也兼容部分 ICY 200 OK 响应。
  if (status_line.startsWith("ICY ")) {
    return status_line.substring(4, 7).toInt();
  }

  const int first_space = status_line.indexOf(' ');
  if (first_space < 0 || first_space + 3 > status_line.length()) return 0;
  return status_line.substring(first_space + 1, first_space + 4).toInt();
}

static String make_redirect_url(const ParsedUrl& current, const String& location)
{
  String loc = location;
  loc.trim();

  if (loc.startsWith("http://") || loc.startsWith("https://")) {
    return loc;
  }

  if (loc.startsWith("/")) {
    String base = "http://" + current.host;
    if (current.port != kDefaultHttpPort) {
      base += ":";
      base += String(current.port);
    }
    return base + loc;
  }

  // 相对路径：按当前目录拼接。
  String dir = current.path;
  const int last_slash = dir.lastIndexOf('/');
  dir = last_slash >= 0 ? dir.substring(0, last_slash + 1) : String("/");

  String base = "http://" + current.host;
  if (current.port != kDefaultHttpPort) {
    base += ":";
    base += String(current.port);
  }
  return base + dir + loc;
}

static bool read_response_header(const ParsedUrl& current, int& status_code, String& location, uint32_t operation_id)
{
  const uint32_t header_started_ms = millis();
  String line;

  auto remaining_timeout_ms = [&]() -> uint32_t {
    const uint32_t elapsed_ms = millis() - header_started_ms;
    return elapsed_ms < kHeaderTimeoutMs ? (kHeaderTimeoutMs - elapsed_ms) : 0;
  };

  uint32_t remaining_ms = remaining_timeout_ms();
  if (remaining_ms == 0 || !read_line_with_timeout(line, remaining_ms, operation_id)) {
    LOGE("[电台] 缺少 HTTP 状态行");
    return false;
  }

  status_code = parse_status_code(line);
  LOGD("[电台] HTTP 状态: %s", line.c_str());

  for (;;) {
    remaining_ms = remaining_timeout_ms();
    if (remaining_ms == 0 || !read_line_with_timeout(line, remaining_ms, operation_id)) {
      break;
    }

    if (line.length() == 0) {
      return true;
    }

    const int colon = line.indexOf(':');
    if (colon > 0) {
      String key = line.substring(0, colon);
      String value = line.substring(colon + 1);
      key.trim();
      value.trim();
      key.toLowerCase();

      if (key == "location") {
        location = make_redirect_url(current, value);
      } else if (key == "content-type") {
        LOGD("[电台] HTTP 内容类型：%s", value.c_str());
      } else if (key == "icy-name") {
        LOGD("[电台] ICY 名称: %s", value.c_str());
      }
    }
  }

  LOGE("[电台] HTTP 头未完整读取，总超时=%lums", (unsigned long)kHeaderTimeoutMs);
  return false;
}

static bool open_http_stream_once(const char* url, uint32_t start_offset, uint32_t operation_id, String& redirect_url)
{
  redirect_url = String();

  if (!operation_is_current(operation_id)) {
    return false;
  }

  ParsedUrl parsed{};
  if (!parse_http_url(url, parsed)) {
    LOGE("[电台] 无效 流 URL: %s", url ? url : "<null>");
    return false;
  }

  LOGD("[电台] HTTP 连接 主机=%s 端口=%u 路径=%s", parsed.host.c_str(), parsed.port, parsed.path.c_str());

  g_client.stop();
  // setTimeout() 的单位是秒，只影响 Stream 读取；TCP connect 使用下方毫秒超时重载。
  g_client.setTimeout((kHeaderTimeoutMs + 999u) / 1000u);

  bool connected = false;
  const uint32_t connect_started_ms = millis();
  while (operation_is_current(operation_id) &&
         WiFi.isConnected() &&
         millis() - connect_started_ms < kConnectTimeoutMs) {
    const uint32_t elapsed_ms = millis() - connect_started_ms;
    const uint32_t remaining_ms = kConnectTimeoutMs > elapsed_ms
        ? (kConnectTimeoutMs - elapsed_ms)
        : 0;
    const uint32_t slice_ms = remaining_ms < kConnectSliceMs ? remaining_ms : kConnectSliceMs;
    if (slice_ms == 0) break;

    g_client.stop();
    if (g_client.connect(parsed.host.c_str(), parsed.port, static_cast<int32_t>(slice_ms))) {
      connected = true;
      break;
    }

    vTaskDelay(pdMS_TO_TICKS(kConnectRetryDelayMs));
  }

  if (!connected) {
    if (!operation_is_current(operation_id)) {
      LOGD("[电台] HTTP 连接已取消：操作=%lu", (unsigned long)operation_id);
    } else {
      LOGE("[电台] HTTP 连接失败 主机=%s 端口=%u", parsed.host.c_str(), parsed.port);
    }
    g_client.stop();
    return false;
  }

  if (!operation_is_current(operation_id)) {
    g_client.stop();
    return false;
  }

  // 使用 HTTP/1.0，避免部分电台 chunked/keep-alive 行为导致流读取不稳定。
  g_client.print("GET ");
  g_client.print(parsed.path);
  g_client.print(" HTTP/1.0\r\n");
  g_client.print("Host: ");
  g_client.print(parsed.host);
  if (parsed.port != kDefaultHttpPort) {
    g_client.print(":");
    g_client.print(parsed.port);
  }
  g_client.print("\r\n");
  g_client.print("User-Agent: ESP32S3-Player/1.0\r\n");
  g_client.print("Accept: audio/mpeg, audio/mp3, */*\r\n");
  if (start_offset > 0) {
    g_client.print("Range: bytes=");
    g_client.print(start_offset);
    g_client.print("-\r\n");
  }
  g_client.print("Icy-MetaData: 0\r\n");
  g_client.print("Connection: close\r\n\r\n");

  int status_code = 0;
  String location;
  if (!read_response_header(parsed, status_code, location, operation_id)) {
    g_client.stop();
    return false;
  }

  if (status_code == 301 || status_code == 302 || status_code == 307 || status_code == 308) {
    if (location.length() == 0) {
      LOGE("[电台] HTTP 重定向缺少 Location");
      g_client.stop();
      return false;
    }

    LOGD("[电台] HTTP 重定向 -> %s", location.c_str());
    redirect_url = location;
    g_client.stop();
    return false;
  }

  if (start_offset > 0 && status_code != 206) {
    LOGW("[电台] Range 起播被服务器拒绝或忽略 状态=%d offset=%lu URL=%s",
         status_code,
         (unsigned long)start_offset,
         url);
    g_client.stop();
    return false;
  }

  if (status_code < 200 || status_code >= 300) {
    LOGE("[电台] HTTP 状态异常=%d URL=%s", status_code, url);
    g_client.stop();
    return false;
  }

  return true;
}

static int http_source_read(void* ctx, uint8_t* dst, size_t bytes)
{
  auto* client = static_cast<WiFiClient*>(ctx);
  if (!client || !dst || bytes == 0) return AUDIO_MP3_SOURCE_ERROR;
  if (!s_open) return AUDIO_MP3_SOURCE_EOF;

  if (s_active_operation_id == 0 || !operation_is_current(s_active_operation_id)) {
    client->stop();
    s_open = false;
    s_active_operation_id = 0;
    publish_source_snapshot(false, false, false, true, 0);
    return AUDIO_MP3_SOURCE_EOF;
  }

  // WiFi 已关闭时直接结束，避免进入底层 socket read。
  // 注意：不要在 available() 之前用 client->connected() 判死，
  // 某些 HTTP/1.0 流在 connected=false 时仍可能有缓冲数据可读。
  if (!WiFi.isConnected()) {
    s_open = false;
    publish_source_snapshot(false, false, false, true, 0);
    return AUDIO_MP3_SOURCE_EOF;
  }

  int avail = client->available();
  if (avail <= 0) {
    if (!client->connected()) {
      s_open = false;
      publish_source_snapshot(false, false, false, true, 0);
      return AUDIO_MP3_SOURCE_EOF;
    }

    // 网络流暂时没数据时不要在 AudioTask 里紧密轮询 available()。
    // 这里短暂让出 CPU0，避免 IDLE0 长时间无法运行导致 task_wdt。
    publish_source_snapshot(true, true, true, false, 0);
    delay(1);
    return AUDIO_MP3_SOURCE_WOULD_BLOCK;
  }

  size_t want = bytes;
  if ((size_t)avail < want) want = (size_t)avail;

  const int n = client->read(dst, want);
  if (n > 0) {
    const int remaining = avail > n ? (avail - n) : 0;
    const bool transport_connected = client->connected() || remaining > 0;
    publish_source_snapshot(true,
                            transport_connected,
                            false,
                            false,
                            static_cast<uint32_t>(remaining),
                            true);
    return n;
  }

  if (!WiFi.isConnected() || !client->connected()) {
    s_open = false;
    publish_source_snapshot(false, false, false, true, 0);
    return AUDIO_MP3_SOURCE_EOF;
  }

  // read() 没拿到数据但连接仍在，给系统一点调度时间。
  publish_source_snapshot(true, true, true, false, 0);
  delay(1);
  return AUDIO_MP3_SOURCE_WOULD_BLOCK;
}

static void http_source_close_impl(void* ctx)
{
  auto* client = static_cast<WiFiClient*>(ctx);
  if (client) {
    client->stop();
  }
  s_open = false;
  s_active_operation_id = 0;
  s_url = String();
  publish_source_snapshot(false, false, false, false, 0);
}

} // namespace

uint32_t audio_mp3_audiotools_source_begin_operation()
{
  uint32_t operation_id = 0;
  portENTER_CRITICAL(&s_operation_mux);
  ++s_operation_generation;
  if (s_operation_generation == 0) {
    s_operation_generation = 1;
  }
  operation_id = s_operation_generation;
  portEXIT_CRITICAL(&s_operation_mux);
  return operation_id;
}

void audio_mp3_audiotools_source_cancel_operation()
{
  (void)audio_mp3_audiotools_source_begin_operation();
}

bool audio_mp3_audiotools_source_operation_is_current(uint32_t operation_id)
{
  return operation_is_current(operation_id);
}

bool audio_mp3_audiotools_source_probe_audio_start_offset(const char* url,
                                                           uint32_t operation_id,
                                                           uint32_t* out_offset)
{
  if (out_offset) *out_offset = 0;
  if (!out_offset || !url || !*url || !operation_is_current(operation_id)) {
    return false;
  }

  String current_url(url);
  bool opened = false;
  for (int attempt = 0; attempt <= kMaxRedirects; ++attempt) {
    String redirect_url;
    opened = open_http_stream_once(current_url.c_str(), 0, operation_id, redirect_url);
    if (opened) break;
    if (redirect_url.length() == 0) break;
    current_url = redirect_url;
  }

  if (!opened) {
    g_client.stop();
    return false;
  }

  uint8_t header[10] = {0};
  const bool read_ok = read_body_exact(header, sizeof(header), kStartupProbeTimeoutMs, operation_id);
  g_client.stop();
  if (!read_ok) {
    LOGW("[网络歌曲] AudioTask 起播 ID3 探测失败，回退普通起播 URL=%s", url);
    return false;
  }

  if (header[0] != 'I' || header[1] != 'D' || header[2] != '3') {
    LOGD("[网络歌曲] AudioTask 起播 ID3 探测：无 ID3v2");
    return true;
  }

  const uint8_t major = header[3];
  const uint8_t flags = header[5];
  const uint32_t tag_size = id3_synchsafe32(header + 6);
  uint32_t offset = 10u + tag_size;
  if (major >= 4 && (flags & 0x10)) {
    offset += 10u;
  }

  if (offset <= 10u || offset > kMaxStartupSkipBytes) {
    LOGW("[网络歌曲] AudioTask 起播 ID3 offset 异常 version=2.%u tag=%lu offset=%lu",
         (unsigned)major,
         (unsigned long)tag_size,
         (unsigned long)offset);
    return false;
  }

  *out_offset = offset;
  LOGI("[网络歌曲] AudioTask 起播跳过 ID3v2.%u tag=%lu offset=%lu",
       (unsigned)major,
       (unsigned long)tag_size,
       (unsigned long)offset);
  return true;
}

bool audio_mp3_audiotools_source_open_from_offset(const char* url, uint32_t start_offset, uint32_t operation_id, AudioMp3Source& out_source)
{
  audio_mp3_audiotools_source_close();
  publish_source_snapshot(false, false, true, false, 0);

  if (!operation_is_current(operation_id)) {
    LOGD("[电台] HTTP 音源打开请求已过期：操作=%lu", (unsigned long)operation_id);
    publish_source_snapshot(false, false, false, false, 0);
    return false;
  }

  if (!url || !*url) {
    LOGE("[电台] HTTP 音源打开失败：URL 为空");
    publish_source_snapshot(false, false, false, false, 0);
    return false;
  }

  if (!WiFi.isConnected()) {
    LOGE("[电台] HTTP 音源打开失败：WiFi 未连接");
    publish_source_snapshot(false, false, false, false, 0);
    return false;
  }

  String current_url(url);
  bool ok = false;

  for (int attempt = 0; attempt <= kMaxRedirects; ++attempt) {
    String redirect_url;
    ok = open_http_stream_once(current_url.c_str(), start_offset, operation_id, redirect_url);
    if (ok) break;

    if (redirect_url.length() == 0) {
      break;
    }

    current_url = redirect_url;
  }

  if (!ok) {
    LOGE("[电台] HTTP 流 打开失败：%s offset=%lu", url, (unsigned long)start_offset);
    publish_source_snapshot(false, false, false, false, 0);
    return false;
  }

  s_open = true;
  s_active_operation_id = operation_id;
  s_url = current_url;

  const int available = g_client.available();
  const bool connected = g_client.connected() || available > 0;
  publish_source_snapshot(true,
                          connected,
                          available <= 0,
                          false,
                          available > 0 ? static_cast<uint32_t>(available) : 0);

  out_source = AudioMp3Source{};
  out_source.ctx = &g_client;
  out_source.read = http_source_read;
  out_source.close = http_source_close_impl;
  out_source.debug_name = s_url.c_str();
  out_source.is_stream = true;

  LOGD("[电台] HTTP 流打开成功：%s offset=%lu", s_url.c_str(), (unsigned long)start_offset);
  return true;
}

bool audio_mp3_audiotools_source_open(const char* url, uint32_t operation_id, AudioMp3Source& out_source)
{
  return audio_mp3_audiotools_source_open_from_offset(url, 0, operation_id, out_source);
}

void audio_mp3_audiotools_source_close()
{
  if (s_open) {
    g_client.stop();
    s_open = false;
  }
  s_active_operation_id = 0;
  s_url = String();
  publish_source_snapshot(false, false, false, false, 0);
}

bool audio_mp3_audiotools_source_get_snapshot(AudioMp3HttpSourceSnapshot* out_snapshot)
{
  if (!out_snapshot) return false;

  portENTER_CRITICAL(&s_snapshot_mux);
  *out_snapshot = s_snapshot;
  portEXIT_CRITICAL(&s_snapshot_mux);
  return true;
}