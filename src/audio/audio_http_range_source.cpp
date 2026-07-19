#include "audio/audio_http_range_source.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/audio_mp3_source_audiotools.h"
#include "utils/log.h"

namespace {

static WiFiClient s_client;
static String s_url;
static uint32_t s_operation_id = 0;
static uint32_t s_logical_pos = 0;
static uint32_t s_network_pos = 0;
static uint32_t s_total_size = 0;
static uint32_t s_range_open_count = 0;
static bool s_open = false;
static bool s_io_error = false;
static bool s_eof = false;
static bool s_reconnecting = false;
static uint8_t s_retry_attempt = 0;
static uint32_t s_retry_delay_ms = 0;
static uint32_t s_reconnect_attempt_count = 0;
static uint32_t s_reconnect_success_count = 0;
static uint32_t s_last_body_data_ms = 0;
static uint32_t s_waiting_since_ms = 0;
static uint32_t s_last_wait_warning_ms = 0;
static uint32_t s_last_buffer_diag_ms = 0;
static char s_last_error[80] = {0};

static uint8_t* s_cache = nullptr;
static uint32_t s_cache_start = 0;
static size_t s_cache_len = 0;
static size_t s_cache_pos = 0;

static portMUX_TYPE s_snapshot_mux = portMUX_INITIALIZER_UNLOCKED;
static AudioHttpRangeSourceSnapshot s_snapshot;

constexpr uint16_t kDefaultHttpPort = 80;
constexpr uint32_t kConnectTimeoutMs = 5000;
constexpr uint32_t kConnectSliceMs = 500;
constexpr uint32_t kConnectRetryDelayMs = 25;
constexpr uint32_t kHeaderTimeoutMs = 6000;
constexpr uint32_t kNoDataTimeoutMs = 12000;
constexpr uint32_t kCacheBytes = 64 * 1024;
constexpr uint32_t kRetryDelaysMs[] = {1000, 2000, 4000, 8000, 15000};
constexpr size_t kRetryDelayCount = sizeof(kRetryDelaysMs) / sizeof(kRetryDelaysMs[0]);
constexpr uint32_t kRetryPollMs = 50;
constexpr uint32_t kBufferDiagIntervalMs = 15000;
constexpr uint32_t kWaitWarningThresholdMs = 2000;
constexpr uint32_t kWaitWarningIntervalMs = 5000;
constexpr int kMaxRedirects = 2;

struct ParsedUrl {
  String host;
  String path;
  uint16_t port = kDefaultHttpPort;
};

struct HttpHeader {
  int status_code = 0;
  String location;
  String content_type;
  String transfer_encoding;
  String content_encoding;
  uint32_t content_length = 0;
  bool content_length_valid = false;
  uint32_t range_start = 0;
  uint32_t range_end = 0;
  uint32_t range_total = 0;
  bool content_range_valid = false;
};

static bool operation_is_current()
{
  return s_operation_id != 0 &&
         audio_mp3_audiotools_source_operation_is_current(s_operation_id);
}

static void clear_error()
{
  s_last_error[0] = '\0';
}

static void set_error(const char* error)
{
  if (!error || !*error) {
    clear_error();
    return;
  }

  strncpy(s_last_error, error, sizeof(s_last_error) - 1);
  s_last_error[sizeof(s_last_error) - 1] = '\0';
}

static void set_http_status_error(int status_code)
{
  snprintf(s_last_error, sizeof(s_last_error), "http_status_%d", status_code);
  s_last_error[sizeof(s_last_error) - 1] = '\0';
}

static bool error_is_retryable(const char* error)
{
  if (!error || !*error) return false;

  if (strcmp(error, "wifi_disconnected") == 0 ||
      strcmp(error, "http_connect_failed") == 0 ||
      strcmp(error, "http_header_timeout") == 0 ||
      strcmp(error, "http_header_disconnected") == 0 ||
      strcmp(error, "http_header_incomplete") == 0 ||
      strcmp(error, "http_range_disconnected_early") == 0 ||
      strcmp(error, "stream_no_data_timeout") == 0) {
    return true;
  }

  static const char kHttpPrefix[] = "http_status_";
  if (strncmp(error, kHttpPrefix, sizeof(kHttpPrefix) - 1) == 0) {
    const int status = atoi(error + sizeof(kHttpPrefix) - 1);
    return status == 408 || status == 425 || status == 429 ||
           status == 500 || status == 502 || status == 503 || status == 504;
  }

  return false;
}

static void update_wait_diagnostics(bool waiting_for_data,
                                    uint32_t cached_available,
                                    uint32_t transport_available)
{
  const uint32_t now = millis();
  if (!waiting_for_data) {
    s_waiting_since_ms = 0;
  } else if (s_waiting_since_ms == 0) {
    s_waiting_since_ms = now;
  }

  if (s_open &&
      (s_last_buffer_diag_ms == 0 ||
       (uint32_t)(now - s_last_buffer_diag_ms) >= kBufferDiagIntervalMs)) {
    s_last_buffer_diag_ms = now;
    LOGD("[NAS FLAC] 缓冲状态：缓存=%lu/%luB TCP=%luB 位置=%lu/%lu Range=%lu 续传=%lu/%lu",
         (unsigned long)cached_available,
         (unsigned long)kCacheBytes,
         (unsigned long)transport_available,
         (unsigned long)s_logical_pos,
         (unsigned long)s_total_size,
         (unsigned long)s_range_open_count,
         (unsigned long)s_reconnect_success_count,
         (unsigned long)s_reconnect_attempt_count);
  }

  if (waiting_for_data && !s_reconnecting && s_waiting_since_ms != 0 &&
      (uint32_t)(now - s_waiting_since_ms) >= kWaitWarningThresholdMs &&
      (s_last_wait_warning_ms == 0 ||
       (uint32_t)(now - s_last_wait_warning_ms) >= kWaitWarningIntervalMs)) {
    s_last_wait_warning_ms = now;
    LOGW("[NAS FLAC] 等待网络数据：已等待=%lums 缓存=%luB TCP=%luB 位置=%lu/%lu",
         (unsigned long)(now - s_waiting_since_ms),
         (unsigned long)cached_available,
         (unsigned long)transport_available,
         (unsigned long)s_logical_pos,
         (unsigned long)s_total_size);
  }
}

static void publish_snapshot(bool waiting_for_data = false)
{
  const int client_available = s_client.available();
  const uint32_t cached_available = s_cache_len > s_cache_pos
      ? static_cast<uint32_t>(s_cache_len - s_cache_pos)
      : 0;
  const uint32_t transport_available = client_available > 0
      ? static_cast<uint32_t>(client_available)
      : 0;

  AudioHttpRangeSourceSnapshot next{};
  next.open = s_open;
  next.transport_connected = s_client.connected() || client_available > 0;
  next.waiting_for_data = waiting_for_data;
  next.reconnecting = s_reconnecting;
  next.eof = s_eof;
  next.retry_attempt = s_retry_attempt;
  next.retry_delay_ms = s_retry_delay_ms;
  next.available_bytes = cached_available + transport_available;
  next.cached_bytes = cached_available;
  next.transport_available_bytes = transport_available;
  next.cache_capacity_bytes = kCacheBytes;
  next.last_data_ms = s_last_body_data_ms;
  next.current_offset = s_logical_pos;
  next.total_size = s_total_size;
  next.range_open_count = s_range_open_count;
  next.reconnect_attempt_count = s_reconnect_attempt_count;
  next.reconnect_success_count = s_reconnect_success_count;

  portENTER_CRITICAL(&s_snapshot_mux);
  s_snapshot = next;
  portEXIT_CRITICAL(&s_snapshot_mux);

  update_wait_diagnostics(waiting_for_data, cached_available, transport_available);
}

static void reset_cache(uint32_t start_offset)
{
  s_cache_start = start_offset;
  s_cache_len = 0;
  s_cache_pos = 0;
}

static bool ensure_cache()
{
  if (s_cache) return true;

  s_cache = static_cast<uint8_t*>(
      heap_caps_malloc(kCacheBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!s_cache) {
    set_error("flac_http_psram_alloc_failed");
    LOGE("[NAS FLAC] HTTP Range PSRAM 缓冲分配失败：%luB",
         (unsigned long)kCacheBytes);
    return false;
  }

  LOGI("[NAS FLAC] HTTP Range 缓冲已分配：%luB PSRAM=%d",
       (unsigned long)kCacheBytes,
       esp_ptr_external_ram(s_cache) ? 1 : 0);
  return true;
}

static bool parse_http_url(const char* url, ParsedUrl& out)
{
  if (!url || !*url) {
    set_error("invalid_url");
    return false;
  }

  String value(url);
  value.trim();
  if (value.startsWith("https://")) {
    set_error("https_not_supported");
    return false;
  }
  if (!value.startsWith("http://")) {
    set_error("unsupported_url_scheme");
    return false;
  }

  String rest = value.substring(7);
  const int slash = rest.indexOf('/');
  String host_port = slash >= 0 ? rest.substring(0, slash) : rest;
  out.path = slash >= 0 ? rest.substring(slash) : String("/");

  const int colon = host_port.lastIndexOf(':');
  if (colon > 0) {
    out.host = host_port.substring(0, colon);
    const int port = host_port.substring(colon + 1).toInt();
    if (port <= 0 || port > 65535) {
      set_error("invalid_url_port");
      return false;
    }
    out.port = static_cast<uint16_t>(port);
  } else {
    out.host = host_port;
    out.port = kDefaultHttpPort;
  }

  if (!out.host.length() || !out.path.length()) {
    set_error("invalid_url");
    return false;
  }
  return true;
}

static int parse_status_code(const String& line)
{
  const int first_space = line.indexOf(' ');
  if (first_space < 0 || first_space + 3 >= line.length()) return 0;
  return line.substring(first_space + 1, first_space + 4).toInt();
}

static String make_redirect_url(const ParsedUrl& current, const String& location)
{
  String loc = location;
  loc.trim();
  if (loc.startsWith("http://") || loc.startsWith("https://")) return loc;

  String base = "http://" + current.host;
  if (current.port != kDefaultHttpPort) {
    base += ":";
    base += String(current.port);
  }

  if (loc.startsWith("/")) return base + loc;

  String dir = current.path;
  const int slash = dir.lastIndexOf('/');
  dir = slash >= 0 ? dir.substring(0, slash + 1) : String("/");
  return base + dir + loc;
}

static bool read_line(String& line)
{
  line = String();
  const uint32_t started_ms = millis();

  while ((uint32_t)(millis() - started_ms) < kHeaderTimeoutMs) {
    if (!operation_is_current()) {
      set_error("cancelled");
      return false;
    }
    if (!WiFi.isConnected()) {
      set_error("wifi_disconnected");
      return false;
    }

    while (s_client.available() > 0) {
      const char c = static_cast<char>(s_client.read());
      if (c == '\r') continue;
      if (c == '\n') return true;
      line += c;
      if (line.length() > 768) {
        set_error("http_header_line_too_long");
        return false;
      }
    }

    if (!s_client.connected()) {
      set_error("http_header_disconnected");
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }

  set_error("http_header_timeout");
  return false;
}

static bool parse_content_range(const String& value,
                                uint32_t& out_start,
                                uint32_t& out_end,
                                uint32_t& out_total)
{
  String text = value;
  text.trim();
  text.toLowerCase();
  if (!text.startsWith("bytes ")) return false;

  const int dash = text.indexOf('-', 6);
  const int slash = text.indexOf('/', dash + 1);
  if (dash < 0 || slash < 0) return false;

  const String start_text = text.substring(6, dash);
  const String end_text = text.substring(dash + 1, slash);
  const String total_text = text.substring(slash + 1);
  if (!start_text.length() || !end_text.length() || !total_text.length() || total_text == "*") {
    return false;
  }

  const unsigned long start = strtoul(start_text.c_str(), nullptr, 10);
  const unsigned long end = strtoul(end_text.c_str(), nullptr, 10);
  const unsigned long total = strtoul(total_text.c_str(), nullptr, 10);
  if (end < start || total == 0 || end >= total) return false;

  out_start = static_cast<uint32_t>(start);
  out_end = static_cast<uint32_t>(end);
  out_total = static_cast<uint32_t>(total);
  return true;
}

static bool read_header(const ParsedUrl& parsed, HttpHeader& header)
{
  String line;
  if (!read_line(line)) return false;

  header.status_code = parse_status_code(line);
  if (header.status_code <= 0) {
    set_error("http_status_invalid");
    return false;
  }

  while (read_line(line)) {
    if (!line.length()) return true;

    const int colon = line.indexOf(':');
    if (colon <= 0) continue;

    String key = line.substring(0, colon);
    String value = line.substring(colon + 1);
    key.trim();
    key.toLowerCase();
    value.trim();

    if (key == "location") {
      header.location = make_redirect_url(parsed, value);
    } else if (key == "content-type") {
      header.content_type = value;
    } else if (key == "content-length") {
      header.content_length = static_cast<uint32_t>(strtoul(value.c_str(), nullptr, 10));
      header.content_length_valid = true;
    } else if (key == "content-range") {
      header.content_range_valid = parse_content_range(value,
                                                       header.range_start,
                                                       header.range_end,
                                                       header.range_total);
    } else if (key == "transfer-encoding") {
      header.transfer_encoding = value;
      header.transfer_encoding.toLowerCase();
    } else if (key == "content-encoding") {
      header.content_encoding = value;
      header.content_encoding.toLowerCase();
    }
  }

  if (!s_last_error[0]) set_error("http_header_incomplete");
  return false;
}

static bool content_type_supported(String content_type)
{
  content_type.trim();
  content_type.toLowerCase();
  const int semicolon = content_type.indexOf(';');
  if (semicolon >= 0) content_type = content_type.substring(0, semicolon);
  content_type.trim();

  return !content_type.length() ||
         content_type == "audio/flac" ||
         content_type == "audio/x-flac" ||
         content_type == "application/octet-stream";
}

static bool connect_once(const char* url,
                         uint32_t start_offset,
                         bool preserve_buffer,
                         String& redirect_url,
                         String& resolved_url)
{
  redirect_url = String();
  resolved_url = String();

  ParsedUrl parsed{};
  if (!parse_http_url(url, parsed)) return false;

  s_client.stop();
  s_client.setTimeout((kHeaderTimeoutMs + 999u) / 1000u);

  bool connected = false;
  const uint32_t started_ms = millis();
  while (operation_is_current() &&
         WiFi.isConnected() &&
         (uint32_t)(millis() - started_ms) < kConnectTimeoutMs) {
    const uint32_t elapsed = millis() - started_ms;
    const uint32_t remaining = elapsed < kConnectTimeoutMs
        ? kConnectTimeoutMs - elapsed
        : 0;
    const uint32_t slice = remaining < kConnectSliceMs ? remaining : kConnectSliceMs;
    if (!slice) break;

    s_client.stop();
    if (s_client.connect(parsed.host.c_str(), parsed.port, static_cast<int32_t>(slice))) {
      connected = true;
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(kConnectRetryDelayMs));
  }

  if (!connected) {
    if (!operation_is_current()) set_error("cancelled");
    else if (!WiFi.isConnected()) set_error("wifi_disconnected");
    else set_error("http_connect_failed");
    s_client.stop();
    return false;
  }

  s_client.print("GET ");
  s_client.print(parsed.path);
  s_client.print(" HTTP/1.0\r\nHost: ");
  s_client.print(parsed.host);
  if (parsed.port != kDefaultHttpPort) {
    s_client.print(":");
    s_client.print(parsed.port);
  }
  s_client.print("\r\nUser-Agent: ESP32S3-Player/1.0\r\n");
  s_client.print("Accept: audio/flac, audio/x-flac, application/octet-stream\r\n");
  s_client.print("Accept-Encoding: identity\r\n");
  s_client.print("Range: bytes=");
  s_client.print(start_offset);
  s_client.print("-\r\nConnection: close\r\n\r\n");

  HttpHeader header{};
  if (!read_header(parsed, header)) {
    s_client.stop();
    return false;
  }

  if (header.status_code == 301 || header.status_code == 302 ||
      header.status_code == 307 || header.status_code == 308) {
    if (!header.location.length()) {
      set_error("redirect_missing_location");
      s_client.stop();
      return false;
    }
    redirect_url = header.location;
    s_client.stop();
    return false;
  }

  if (header.status_code != 206) {
    if (header.status_code >= 200 && header.status_code < 300) {
      set_error("http_range_not_supported");
    } else {
      set_http_status_error(header.status_code);
    }
    s_client.stop();
    return false;
  }

  if (!header.content_range_valid ||
      header.range_start != start_offset ||
      header.range_total == 0) {
    set_error("http_content_range_invalid");
    s_client.stop();
    return false;
  }

  if (header.transfer_encoding.length() && header.transfer_encoding != "identity") {
    set_error("http_transfer_encoding_unsupported");
    s_client.stop();
    return false;
  }

  if (header.content_encoding.length() && header.content_encoding != "identity") {
    set_error("http_content_encoding_unsupported");
    s_client.stop();
    return false;
  }

  if (!content_type_supported(header.content_type)) {
    set_error("flac_content_type_unsupported");
    LOGE("[NAS FLAC] 不支持的 Content-Type：%s", header.content_type.c_str());
    s_client.stop();
    return false;
  }

  if (s_total_size != 0 && s_total_size != header.range_total) {
    set_error("http_file_size_changed");
    s_client.stop();
    return false;
  }

  s_total_size = header.range_total;
  s_network_pos = start_offset;
  if (!preserve_buffer) {
    s_logical_pos = start_offset;
    reset_cache(start_offset);
  }
  s_last_body_data_ms = 0;
  s_eof = start_offset >= s_total_size;
  ++s_range_open_count;
  resolved_url = url;
  clear_error();
  publish_snapshot(true);

  LOGD("[NAS FLAC] Range 已打开：offset=%lu total=%lu type=%s 次数=%lu",
       (unsigned long)start_offset,
       (unsigned long)s_total_size,
       header.content_type.c_str(),
       (unsigned long)s_range_open_count);
  return true;
}

static bool open_range(const char* url,
                       uint32_t start_offset,
                       bool preserve_buffer = false)
{
  String current_url(url ? url : "");
  for (int redirect = 0; redirect <= kMaxRedirects; ++redirect) {
    String redirect_url;
    String resolved_url;
    if (connect_once(current_url.c_str(),
                     start_offset,
                     preserve_buffer,
                     redirect_url,
                     resolved_url)) {
      s_url = resolved_url;
      return true;
    }
    if (!redirect_url.length()) return false;
    current_url = redirect_url;
  }

  set_error("too_many_redirects");
  return false;
}

static bool wait_before_retry(uint32_t delay_ms)
{
  const uint32_t started_ms = millis();
  while ((uint32_t)(millis() - started_ms) < delay_ms) {
    if (!operation_is_current()) {
      set_error("cancelled");
      return false;
    }

    const uint32_t elapsed = millis() - started_ms;
    s_retry_delay_ms = elapsed < delay_ms ? delay_ms - elapsed : 0;
    publish_snapshot(true);

    const uint32_t remaining = elapsed < delay_ms ? delay_ms - elapsed : 0;
    const uint32_t slice = remaining < kRetryPollMs ? remaining : kRetryPollMs;
    if (slice == 0) break;
    vTaskDelay(pdMS_TO_TICKS(slice));
  }

  s_retry_delay_ms = 0;
  return operation_is_current();
}

static bool retry_range_transport(const char* trigger_error)
{
  const uint32_t resume_offset = s_network_pos;
  char cause[80] = {0};
  if (trigger_error && *trigger_error) {
    strncpy(cause, trigger_error, sizeof(cause) - 1);
  } else {
    strncpy(cause, "http_transport_error", sizeof(cause) - 1);
  }

  if (!error_is_retryable(cause)) {
    set_error(cause);
    s_io_error = true;
    return false;
  }

  LOGW("[NAS FLAC] 网络中断：原因=%s 解码位置=%lu 网络位置=%lu 缓存=%luB",
       cause,
       (unsigned long)s_logical_pos,
       (unsigned long)resume_offset,
       (unsigned long)(s_cache_len > s_cache_pos ? s_cache_len - s_cache_pos : 0));

  s_client.stop();
  s_reconnecting = true;
  s_retry_attempt = 0;
  s_retry_delay_ms = 0;
  publish_snapshot(true);

  for (size_t i = 0; i < kRetryDelayCount; ++i) {
    if (!operation_is_current()) {
      set_error("cancelled");
      s_reconnecting = false;
      s_retry_attempt = 0;
      s_retry_delay_ms = 0;
      s_io_error = true;
      publish_snapshot(false);
      return false;
    }

    s_retry_attempt = static_cast<uint8_t>(i + 1);
    s_retry_delay_ms = kRetryDelaysMs[i];
    ++s_reconnect_attempt_count;
    LOGW("[NAS FLAC] 准备 Range 续传：尝试=%u/%u 延迟=%lums offset=%lu 原因=%s",
         (unsigned)s_retry_attempt,
         (unsigned)kRetryDelayCount,
         (unsigned long)kRetryDelaysMs[i],
         (unsigned long)resume_offset,
         cause);
    publish_snapshot(true);

    if (!wait_before_retry(kRetryDelaysMs[i])) {
      set_error("cancelled");
      s_reconnecting = false;
      s_retry_attempt = 0;
      s_retry_delay_ms = 0;
      s_io_error = true;
      publish_snapshot(false);
      return false;
    }

    if (open_range(s_url.c_str(), resume_offset, true)) {
      ++s_reconnect_success_count;
      s_reconnecting = false;
      s_retry_attempt = 0;
      s_retry_delay_ms = 0;
      s_io_error = false;
      clear_error();
      LOGI("[NAS FLAC] Range 续传成功：offset=%lu 尝试=%u 总成功=%lu Range次数=%lu",
           (unsigned long)resume_offset,
           (unsigned)(i + 1),
           (unsigned long)s_reconnect_success_count,
           (unsigned long)s_range_open_count);
      publish_snapshot(true);
      return true;
    }

    const char* retry_error = s_last_error[0] ? s_last_error : "http_reconnect_failed";
    strncpy(cause, retry_error, sizeof(cause) - 1);
    cause[sizeof(cause) - 1] = '\0';
    LOGW("[NAS FLAC] Range 续传失败：尝试=%u/%u offset=%lu 错误=%s WiFi=%d",
         (unsigned)(i + 1),
         (unsigned)kRetryDelayCount,
         (unsigned long)resume_offset,
         cause,
         WiFi.isConnected() ? 1 : 0);

    if (!error_is_retryable(cause)) break;
  }

  LOGE("[NAS FLAC] Range 续传失败达到上限：offset=%lu 尝试总数=%lu 最后错误=%s",
       (unsigned long)resume_offset,
       (unsigned long)s_reconnect_attempt_count,
       cause);
  set_error("flac_reconnect_exhausted");
  s_reconnecting = false;
  s_retry_attempt = 0;
  s_retry_delay_ms = 0;
  s_io_error = true;
  publish_snapshot(false);
  return false;
}

static bool fill_cache(size_t minimum_bytes)
{
  if (!s_open || !s_cache) return false;
  if (s_logical_pos >= s_total_size) {
    s_eof = true;
    publish_snapshot(false);
    return true;
  }

  reset_cache(s_logical_pos);
  const size_t file_remaining = static_cast<size_t>(s_total_size - s_logical_pos);
  if (minimum_bytes > file_remaining) minimum_bytes = file_remaining;
  if (minimum_bytes > kCacheBytes) minimum_bytes = kCacheBytes;

  uint32_t wait_started_ms = millis();
  while (s_cache_len < minimum_bytes) {
    if (!operation_is_current()) {
      set_error("cancelled");
      s_io_error = true;
      publish_snapshot(false);
      return false;
    }
    if (!WiFi.isConnected()) {
      set_error("wifi_disconnected");
      if (!retry_range_transport(s_last_error)) return false;
      wait_started_ms = millis();
      continue;
    }

    const int available = s_client.available();
    if (available > 0) {
      size_t want = kCacheBytes - s_cache_len;
      if (static_cast<size_t>(available) < want) want = static_cast<size_t>(available);
      const size_t remaining = static_cast<size_t>(s_total_size - s_network_pos);
      if (remaining < want) want = remaining;
      if (!want) break;

      const int read_count = s_client.read(s_cache + s_cache_len, want);
      if (read_count > 0) {
        s_cache_len += static_cast<size_t>(read_count);
        s_network_pos += static_cast<uint32_t>(read_count);
        s_last_body_data_ms = millis();
        s_io_error = false;
        publish_snapshot(false);
        continue;
      }
    }

    if (!s_client.connected() && s_client.available() <= 0) {
      if (s_network_pos >= s_total_size) {
        s_eof = true;
        break;
      }
      set_error("http_range_disconnected_early");
      if (!retry_range_transport(s_last_error)) return false;
      wait_started_ms = millis();
      continue;
    }

    const uint32_t activity_base = s_last_body_data_ms
        ? s_last_body_data_ms
        : wait_started_ms;
    if ((uint32_t)(millis() - activity_base) >= kNoDataTimeoutMs) {
      set_error("stream_no_data_timeout");
      if (!retry_range_transport(s_last_error)) return false;
      wait_started_ms = millis();
      continue;
    }

    publish_snapshot(true);
    vTaskDelay(pdMS_TO_TICKS(1));
  }

  // 已满足本次读取后，顺手吸收 TCP 缓冲中已经到达的数据，不额外等待。
  while (s_cache_len < kCacheBytes && s_network_pos < s_total_size) {
    const int available = s_client.available();
    if (available <= 0) break;

    size_t want = kCacheBytes - s_cache_len;
    if (static_cast<size_t>(available) < want) want = static_cast<size_t>(available);
    const size_t remaining = static_cast<size_t>(s_total_size - s_network_pos);
    if (remaining < want) want = remaining;
    if (!want) break;

    const int read_count = s_client.read(s_cache + s_cache_len, want);
    if (read_count <= 0) break;
    s_cache_len += static_cast<size_t>(read_count);
    s_network_pos += static_cast<uint32_t>(read_count);
    s_last_body_data_ms = millis();
  }

  publish_snapshot(false);
  return s_cache_len >= minimum_bytes;
}

}  // namespace

bool audio_http_range_source_open(const char* url, uint32_t operation_id)
{
  audio_http_range_source_close();
  clear_error();
  s_operation_id = operation_id;
  s_io_error = false;
  s_eof = false;
  s_reconnecting = false;
  s_retry_attempt = 0;
  s_retry_delay_ms = 0;
  s_reconnect_attempt_count = 0;
  s_reconnect_success_count = 0;
  s_waiting_since_ms = 0;
  s_last_wait_warning_ms = 0;
  s_last_buffer_diag_ms = 0;
  s_total_size = 0;
  s_range_open_count = 0;

  if (!operation_is_current()) {
    set_error("cancelled");
    return false;
  }
  if (!WiFi.isConnected()) {
    set_error("wifi_disconnected");
    return false;
  }
  if (!ensure_cache()) return false;

  s_open = true;
  if (!open_range(url, 0)) {
    s_open = false;
    s_io_error = true;
    publish_snapshot(false);
    return false;
  }

  LOGI("[NAS FLAC] HTTP Range 音源打开成功：size=%lu URL=%s",
       (unsigned long)s_total_size,
       s_url.c_str());
  return true;
}

void audio_http_range_source_close()
{
  if (s_open && (s_reconnect_attempt_count > 0 || s_range_open_count > 1)) {
    LOGI("[NAS FLAC] Range 音源关闭统计：Range=%lu 续传尝试=%lu 成功=%lu 最终位置=%lu/%lu",
         (unsigned long)s_range_open_count,
         (unsigned long)s_reconnect_attempt_count,
         (unsigned long)s_reconnect_success_count,
         (unsigned long)s_logical_pos,
         (unsigned long)s_total_size);
  }

  s_client.stop();
  s_url = String();
  s_operation_id = 0;
  s_logical_pos = 0;
  s_network_pos = 0;
  s_total_size = 0;
  s_range_open_count = 0;
  s_open = false;
  s_io_error = false;
  s_eof = false;
  s_reconnecting = false;
  s_retry_attempt = 0;
  s_retry_delay_ms = 0;
  s_reconnect_attempt_count = 0;
  s_reconnect_success_count = 0;
  s_last_body_data_ms = 0;
  s_waiting_since_ms = 0;
  s_last_wait_warning_ms = 0;
  s_last_buffer_diag_ms = 0;
  reset_cache(0);

  // 64KB PSRAM 缓冲在首次分配后常驻复用，避免连续切歌反复申请/释放造成碎片。
  publish_snapshot(false);
}

ssize_t audio_http_range_source_read(void* dst, size_t bytes)
{
  if (!dst || bytes == 0) return 0;
  if (!s_open) {
    set_error("source_not_open");
    s_io_error = true;
    return -1;
  }
  if (s_io_error) {
    return -1;
  }
  if (s_logical_pos >= s_total_size) {
    s_eof = true;
    publish_snapshot(false);
    return 0;
  }

  uint8_t* output = static_cast<uint8_t*>(dst);
  size_t copied = 0;

  while (copied < bytes && s_logical_pos < s_total_size) {
    if (s_cache_pos < s_cache_len) {
      size_t available = s_cache_len - s_cache_pos;
      size_t want = bytes - copied;
      if (available < want) want = available;
      memcpy(output + copied, s_cache + s_cache_pos, want);
      copied += want;
      s_cache_pos += want;
      s_logical_pos += static_cast<uint32_t>(want);
      continue;
    }

    const size_t remaining_request = bytes - copied;
    if (!fill_cache(remaining_request)) {
      return copied > 0 ? static_cast<ssize_t>(copied) : -1;
    }
    if (s_cache_len == 0) break;
  }

  if (s_logical_pos >= s_total_size) s_eof = true;
  publish_snapshot(false);
  return static_cast<ssize_t>(copied);
}

bool audio_http_range_source_seek(uint32_t absolute_offset)
{
  if (!s_open || s_io_error || absolute_offset > s_total_size) {
    if (!s_io_error) set_error("seek_out_of_range");
    s_io_error = true;
    return false;
  }

  if (absolute_offset == s_logical_pos) return true;

  const uint32_t cache_end = s_cache_start + static_cast<uint32_t>(s_cache_len);
  if (absolute_offset >= s_cache_start && absolute_offset <= cache_end) {
    s_cache_pos = static_cast<size_t>(absolute_offset - s_cache_start);
    s_logical_pos = absolute_offset;
    s_eof = absolute_offset >= s_total_size;
    publish_snapshot(false);
    return true;
  }

  if (absolute_offset == s_total_size) {
    s_client.stop();
    s_logical_pos = absolute_offset;
    s_network_pos = absolute_offset;
    s_eof = true;
    reset_cache(absolute_offset);
    publish_snapshot(false);
    return true;
  }

  if (!open_range(s_url.c_str(), absolute_offset)) {
    s_io_error = true;
    publish_snapshot(false);
    return false;
  }

  return true;
}

uint32_t audio_http_range_source_tell()
{
  return s_logical_pos;
}

uint32_t audio_http_range_source_size()
{
  return s_total_size;
}

bool audio_http_range_source_had_io_error()
{
  return s_io_error;
}

bool audio_http_range_source_is_open()
{
  return s_open;
}

const char* audio_http_range_source_get_last_error()
{
  return s_last_error;
}

bool audio_http_range_source_get_snapshot(AudioHttpRangeSourceSnapshot* out_snapshot)
{
  if (!out_snapshot) return false;
  portENTER_CRITICAL(&s_snapshot_mux);
  *out_snapshot = s_snapshot;
  portEXIT_CRITICAL(&s_snapshot_mux);
  return true;
}
