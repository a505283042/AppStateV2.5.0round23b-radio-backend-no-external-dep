#include "audio/audio_http_range_source.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <esp_heap_caps.h>
#include "app_diagnostics.h"
#if APP_DIAG_NAS_FLAC_PERFORMANCE
#include <esp_timer.h>
#endif
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <stdio.h>
#include <ctype.h>
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
static bool s_prefetch_eof = false;
static bool s_reconnecting = false;
static bool s_transport_connected = false;
static uint32_t s_transport_available_bytes = 0;
static uint8_t s_retry_attempt = 0;
static uint32_t s_retry_delay_ms = 0;
static uint32_t s_reconnect_attempt_count = 0;
static uint32_t s_reconnect_success_count = 0;
static uint32_t s_transport_opened_ms = 0;
static uint32_t s_last_body_data_ms = 0;
static uint32_t s_waiting_since_ms = 0;
static uint32_t s_last_wait_warning_ms = 0;
static uint32_t s_last_buffer_diag_ms = 0;
// FLAC网络缓存诊断：记录峰值、耗尽次数，定位NAS抖动还是其它环节卡顿。
static size_t s_diag_ring_peak_bytes = 0;
static uint32_t s_diag_ring_empty_count = 0;
static uint32_t s_diag_ring_empty_start_ms = 0;
static uint32_t s_diag_ring_empty_total_ms = 0;
// 解码读取回调的累计等待诊断，用于把“解码总耗时”拆成纯解码与网络等待。
static uint32_t s_diag_reader_wait_count = 0;
static uint64_t s_diag_reader_wait_total_us = 0;
static uint32_t s_diag_low_watermark_count = 0;
static size_t s_diag_min_cached_bytes = 0;
static bool s_diag_low_water_active = false;
static bool s_diag_playback_tracking_started = false;
static char s_last_error[80] = {0};

// FLAC 网络输入改为生产者/消费者模型：网络任务只写环形缓冲，AudioTask 只读。
// 这样 NAS 或 Wi-Fi 的短时抖动不会直接阻塞 FLAC 解码回调。
static uint8_t* s_ring = nullptr;
static size_t s_ring_capacity_bytes = 0;
static size_t s_ring_read = 0;
static size_t s_ring_write = 0;
static size_t s_ring_count = 0;
static uint32_t s_ring_generation = 1;
static uint32_t s_prefetch_target_bytes = 0;

static TaskHandle_t s_prefetch_task = nullptr;
static volatile bool s_stop_requested = false;
static volatile bool s_seek_pending = false;
static bool s_prefetch_running = false;
static uint32_t s_seek_offset = 0;
static uint32_t s_seek_request_id = 0;
static uint32_t s_seek_completed_id = 0;
static bool s_seek_result = false;

static StaticSemaphore_t s_state_mu_buf;
static SemaphoreHandle_t s_state_mu = nullptr;
static portMUX_TYPE s_snapshot_mux = portMUX_INITIALIZER_UNLOCKED;
static AudioHttpRangeSourceSnapshot s_snapshot;

constexpr uint16_t kDefaultHttpPort = 80;
constexpr uint32_t kConnectTimeoutMs = 5000;
constexpr uint32_t kConnectSliceMs = 500;
constexpr uint32_t kConnectRetryDelayMs = 25;
constexpr uint32_t kHeaderTimeoutMs = 6000;
constexpr uint32_t kNoDataTimeoutMs = 12000;
// 高码率 FLAC 优先使用 512KB PSRAM 环形缓冲；PSRAM 紧张时自动回退 256KB。
constexpr uint32_t kRingPreferredBytes = 512 * 1024;
constexpr uint32_t kRingFallbackBytes = 256 * 1024;
constexpr uint32_t kRingGuardBytes = 32 * 1024;
constexpr uint32_t kStartupPrefillBytes = 96 * 1024;
constexpr uint32_t kStartupMinimumBytes = 16 * 1024;
constexpr uint32_t kStartupPrefillTimeoutMs = 6000;
constexpr uint32_t kDefaultPrefetchTargetBytes = 224 * 1024;
constexpr uint32_t kHighRatePrefetchTargetBytes = 448 * 1024;
// 高码率流一次最多搬运 16KB，减少 socket/read 与互斥切换次数。
constexpr uint32_t kPrefetchReadChunkBytes = 16 * 1024;
constexpr uint32_t kLowWatermarkBytes = 32 * 1024;
constexpr uint32_t kLowWatermarkRecoveryBytes = 64 * 1024;
constexpr uint32_t kSeekTimeoutMs = 15000;
constexpr uint32_t kStopTimeoutMs = 2500;  // 必须小于 AudioTask 的停止命令超时
constexpr uint32_t kPrefetchTaskStackBytes = 8192;
constexpr UBaseType_t kPrefetchTaskPrio = 4;
// AudioTask 固定 core0；网络预取迁到 core1，避免 96kHz 解码持续压制生产者。
constexpr BaseType_t kPrefetchTaskCore = 1;
constexpr uint32_t kRetryDelaysMs[] = {1000, 2000, 4000, 8000, 15000};
constexpr size_t kRetryDelayCount = sizeof(kRetryDelaysMs) / sizeof(kRetryDelaysMs[0]);
constexpr uint32_t kRetryPollMs = 50;
constexpr uint32_t kBufferDiagIntervalMs = 15000;
constexpr uint32_t kWaitWarningThresholdMs = 500;
constexpr uint32_t kWaitWarningIntervalMs = 5000;
constexpr int kMaxRedirects = 2;

static uint32_t ring_capacity_bytes()
{
  return s_ring_capacity_bytes > 0
      ? static_cast<uint32_t>(s_ring_capacity_bytes)
      : kRingFallbackBytes;
}

static uint32_t clamp_prefetch_target(uint32_t requested)
{
  const uint32_t capacity = ring_capacity_bytes();
  const uint32_t maximum = capacity > kRingGuardBytes
      ? capacity - kRingGuardBytes
      : capacity;
  return requested < maximum ? requested : maximum;
}

static uint32_t active_prefetch_target_bytes()
{
  const uint32_t requested = s_prefetch_target_bytes > 0
      ? s_prefetch_target_bytes
      : kDefaultPrefetchTargetBytes;
  return clamp_prefetch_target(requested);
}

static void update_low_watermark_locked(size_t cached_bytes)
{
#if APP_DIAG_NAS_FLAC_PERFORMANCE
  if (!s_diag_playback_tracking_started) return;

  if (cached_bytes < s_diag_min_cached_bytes) {
    s_diag_min_cached_bytes = cached_bytes;
  }

  if (cached_bytes < kLowWatermarkBytes) {
    if (!s_diag_low_water_active) {
      s_diag_low_water_active = true;
      ++s_diag_low_watermark_count;
    }
  } else if (cached_bytes >= kLowWatermarkRecoveryBytes) {
    s_diag_low_water_active = false;
  }
#else
  (void)cached_bytes;
#endif
}

static void record_reader_wait_locked(uint64_t waited_us)
{
#if APP_DIAG_NAS_FLAC_PERFORMANCE
  if (!s_diag_playback_tracking_started || waited_us == 0) return;
  ++s_diag_reader_wait_count;
  s_diag_reader_wait_total_us += waited_us;
#else
  (void)waited_us;
#endif
}

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

static SemaphoreHandle_t state_mutex()
{
  if (!s_state_mu) {
    s_state_mu = xSemaphoreCreateMutexStatic(&s_state_mu_buf);
  }
  return s_state_mu;
}

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

static void update_wait_diagnostics(bool source_open,
                                    bool waiting_for_data,
                                    uint32_t cached_available,
                                    uint32_t transport_available,
                                    uint32_t logical_pos,
                                    uint32_t total_size,
                                    uint32_t range_open_count,
                                    uint32_t reconnect_success_count,
                                    uint32_t reconnect_attempt_count,
                                    bool reconnecting)
{
  const uint32_t now = millis();
  bool log_buffer = false;
  bool log_wait = false;
  uint32_t waited_ms = 0;

  SemaphoreHandle_t mu = state_mutex();
  if (!mu || xSemaphoreTake(mu, pdMS_TO_TICKS(50)) != pdTRUE) return;

  if (!waiting_for_data) {
    s_waiting_since_ms = 0;
  } else if (s_waiting_since_ms == 0) {
    s_waiting_since_ms = now;
  }

  if (source_open &&
      (s_last_buffer_diag_ms == 0 ||
       (uint32_t)(now - s_last_buffer_diag_ms) >= kBufferDiagIntervalMs)) {
    s_last_buffer_diag_ms = now;
    log_buffer = true;
  }

  if (waiting_for_data && !reconnecting && s_waiting_since_ms != 0 &&
      (uint32_t)(now - s_waiting_since_ms) >= kWaitWarningThresholdMs &&
      (s_last_wait_warning_ms == 0 ||
       (uint32_t)(now - s_last_wait_warning_ms) >= kWaitWarningIntervalMs)) {
    s_last_wait_warning_ms = now;
    waited_ms = now - s_waiting_since_ms;
    log_wait = true;
  }
  xSemaphoreGive(mu);

  if (cached_available > s_diag_ring_peak_bytes) {
    s_diag_ring_peak_bytes = cached_available;
  }
  if (cached_available == 0) {
    if (s_diag_ring_empty_start_ms == 0) {
      s_diag_ring_empty_start_ms = now;
      ++s_diag_ring_empty_count;
    }
  } else if (s_diag_ring_empty_start_ms != 0) {
    s_diag_ring_empty_total_ms += now - s_diag_ring_empty_start_ms;
    s_diag_ring_empty_start_ms = 0;
  }

  if (log_buffer) {
    LOGD("[NAS FLAC] 环形缓冲状态：缓存=%lu/%luB 峰值=%luB 耗尽=%lu 次 等待=%lums TCP=%luB 位置=%lu/%lu Range=%lu 续传=%lu/%lu",
         (unsigned long)cached_available,
         (unsigned long)ring_capacity_bytes(),
         (unsigned long)s_diag_ring_peak_bytes,
         (unsigned long)s_diag_ring_empty_count,
         (unsigned long)s_diag_ring_empty_total_ms,
         (unsigned long)transport_available,
         (unsigned long)logical_pos,
         (unsigned long)total_size,
         (unsigned long)range_open_count,
         (unsigned long)reconnect_success_count,
         (unsigned long)reconnect_attempt_count);
  }

  if (log_wait) {
    LOGW("[NAS FLAC] 环形缓冲耗尽，等待网络数据：已等待=%lums 缓存=%luB TCP=%luB 位置=%lu/%lu",
         (unsigned long)waited_ms,
         (unsigned long)cached_available,
         (unsigned long)transport_available,
         (unsigned long)logical_pos,
         (unsigned long)total_size);
  }
}

static void publish_snapshot(bool waiting_for_data = false)
{
  bool open = false;
  bool connected = false;
  bool reconnecting = false;
  bool eof = false;
  bool prefetch_complete = false;
  bool io_error = false;
  uint8_t retry_attempt = 0;
  uint32_t retry_delay_ms = 0;
  uint32_t cached_available = 0;
  uint32_t transport_available = 0;
  uint32_t last_data_ms = 0;
  uint32_t logical_pos = 0;
  uint32_t total_size = 0;
  uint32_t range_open_count = 0;
  uint32_t reconnect_attempt_count = 0;
  uint32_t reconnect_success_count = 0;
  uint32_t reader_wait_count = 0;
  uint64_t reader_wait_total_us = 0;
  uint32_t low_watermark_count = 0;
  uint32_t min_cached_bytes = 0;

  SemaphoreHandle_t mu = state_mutex();
  if (!mu || xSemaphoreTake(mu, pdMS_TO_TICKS(100)) != pdTRUE) {
    return;
  }
  open = s_open;
  connected = s_transport_connected;
  reconnecting = s_reconnecting;
  eof = s_eof;
  prefetch_complete = s_prefetch_eof;
  io_error = s_io_error;
  retry_attempt = s_retry_attempt;
  retry_delay_ms = s_retry_delay_ms;
  cached_available = static_cast<uint32_t>(s_ring_count);
  transport_available = s_transport_available_bytes;
  last_data_ms = s_last_body_data_ms;
  logical_pos = s_logical_pos;
  total_size = s_total_size;
  range_open_count = s_range_open_count;
  reconnect_attempt_count = s_reconnect_attempt_count;
  reconnect_success_count = s_reconnect_success_count;
  reader_wait_count = s_diag_reader_wait_count;
  reader_wait_total_us = s_diag_reader_wait_total_us;
  low_watermark_count = s_diag_low_watermark_count;
  min_cached_bytes = static_cast<uint32_t>(s_diag_min_cached_bytes);
  xSemaphoreGive(mu);

  AudioHttpRangeSourceSnapshot next{};
  next.open = open;
  next.transport_connected = connected;
  const bool effective_waiting = waiting_for_data ||
      (open && cached_available == 0 && !prefetch_complete && !io_error);
  next.waiting_for_data = effective_waiting;
  next.reconnecting = reconnecting;
  next.eof = eof;
  next.prefetch_complete = prefetch_complete;
  next.retry_attempt = retry_attempt;
  next.retry_delay_ms = retry_delay_ms;
  next.available_bytes = cached_available + transport_available;
  next.cached_bytes = cached_available;
  next.transport_available_bytes = transport_available;
  next.cache_capacity_bytes = ring_capacity_bytes();
  next.last_data_ms = last_data_ms;
  next.current_offset = logical_pos;
  next.total_size = total_size;
  next.range_open_count = range_open_count;
  next.reconnect_attempt_count = reconnect_attempt_count;
  next.reconnect_success_count = reconnect_success_count;
  next.reader_wait_count = reader_wait_count;
  next.reader_wait_total_us = reader_wait_total_us;
  next.low_watermark_count = low_watermark_count;
  next.min_cached_bytes = min_cached_bytes;

  portENTER_CRITICAL(&s_snapshot_mux);
  s_snapshot = next;
  portEXIT_CRITICAL(&s_snapshot_mux);

  update_wait_diagnostics(open,
                          effective_waiting,
                          cached_available,
                          transport_available,
                          logical_pos,
                          total_size,
                          range_open_count,
                          reconnect_success_count,
                          reconnect_attempt_count,
                          reconnecting);
}

static void reset_ring_locked(uint32_t start_offset)
{
  s_ring_read = 0;
  s_ring_write = 0;
  s_ring_count = 0;
  ++s_ring_generation;
  if (s_ring_generation == 0) s_ring_generation = 1;
  s_logical_pos = start_offset;
  s_eof = start_offset >= s_total_size && s_total_size != 0;
}

static bool ensure_ring()
{
  if (s_ring) return true;

  size_t requested = kRingPreferredBytes;
  s_ring = static_cast<uint8_t*>(
      heap_caps_malloc(requested, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!s_ring) {
    requested = kRingFallbackBytes;
    s_ring = static_cast<uint8_t*>(
        heap_caps_malloc(requested, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  if (!s_ring) {
    set_error("flac_http_psram_alloc_failed");
    LOGE("[NAS FLAC] HTTP 环形缓冲分配失败：首选=%luB 回退=%luB",
         (unsigned long)kRingPreferredBytes,
         (unsigned long)kRingFallbackBytes);
    return false;
  }

  s_ring_capacity_bytes = requested;
  s_prefetch_target_bytes = clamp_prefetch_target(kDefaultPrefetchTargetBytes);
  LOGI("[NAS FLAC] HTTP 环形缓冲已分配：%luB PSRAM=%d 模式=%s",
       (unsigned long)s_ring_capacity_bytes,
       esp_ptr_external_ram(s_ring) ? 1 : 0,
       s_ring_capacity_bytes >= kRingPreferredBytes ? "高码率" : "兼容回退");
  return true;
}

static bool url_path_percent_encoding_valid(const String& path);

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
  if (!url_path_percent_encoding_valid(out.path)) {
    set_error("invalid_url_percent_encoding");
    LOGE("[NAS FLAC] URL 百分号编码不完整：长度=%u；请检查上游路径是否被截断",
         (unsigned)value.length());
    return false;
  }
  return true;
}

static bool url_path_percent_encoding_valid(const String& path)
{
  for (size_t i = 0; i < path.length(); ++i) {
    if (path[i] != '%') continue;
    if (i + 2 >= path.length() ||
        !isxdigit(static_cast<unsigned char>(path[i + 1])) ||
        !isxdigit(static_cast<unsigned char>(path[i + 2]))) {
      return false;
    }
    i += 2;
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
    if (s_stop_requested || !operation_is_current()) {
      set_error("cancelled");
      return false;
    }
    if (s_seek_pending) {
      set_error("seek_interrupted_transport");
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
  while (!s_stop_requested &&
         !s_seek_pending &&
         operation_is_current() &&
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

  SemaphoreHandle_t mu = state_mutex();
  if (!mu || xSemaphoreTake(mu, pdMS_TO_TICKS(100)) != pdTRUE) {
    set_error("flac_prefetch_mutex_failed");
    s_client.stop();
    return false;
  }
  if (s_total_size != 0 && s_total_size != header.range_total) {
    xSemaphoreGive(mu);
    set_error("http_file_size_changed");
    s_client.stop();
    return false;
  }

  const int transport_available = s_client.available();
  s_total_size = header.range_total;
  s_network_pos = start_offset;
  s_transport_connected = true;
  s_transport_available_bytes = transport_available > 0
      ? static_cast<uint32_t>(transport_available)
      : 0;
  s_transport_opened_ms = millis();
  s_last_body_data_ms = 0;
  s_prefetch_eof = start_offset >= s_total_size;
  ++s_range_open_count;
  const uint32_t range_open_count = s_range_open_count;
  xSemaphoreGive(mu);

  resolved_url = url;
  clear_error();
  publish_snapshot(true);

  LOGD("[NAS FLAC] Range 已打开：offset=%lu total=%lu type=%s 次数=%lu",
       (unsigned long)start_offset,
       (unsigned long)header.range_total,
       header.content_type.c_str(),
       (unsigned long)range_open_count);
  return true;
}

static bool open_range(const char* url,
                       uint32_t start_offset)
{
  String current_url(url ? url : "");
  for (int redirect = 0; redirect <= kMaxRedirects; ++redirect) {
    String redirect_url;
    String resolved_url;
    if (connect_once(current_url.c_str(),
                     start_offset,
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


static bool control_request_pending()
{
  return s_stop_requested || s_seek_pending || !operation_is_current();
}

static bool wait_before_retry(uint32_t delay_ms)
{
  const uint32_t started_ms = millis();
  while ((uint32_t)(millis() - started_ms) < delay_ms) {
    if (s_stop_requested || !operation_is_current()) {
      set_error("cancelled");
      return false;
    }
    if (s_seek_pending) {
      set_error("seek_interrupted_reconnect");
      return false;
    }

    const uint32_t elapsed = millis() - started_ms;
    SemaphoreHandle_t mu = state_mutex();
    if (mu && xSemaphoreTake(mu, pdMS_TO_TICKS(50)) == pdTRUE) {
      s_retry_delay_ms = elapsed < delay_ms ? delay_ms - elapsed : 0;
      xSemaphoreGive(mu);
    }
    publish_snapshot(true);

    const uint32_t remaining = elapsed < delay_ms ? delay_ms - elapsed : 0;
    const uint32_t slice = remaining < kRetryPollMs ? remaining : kRetryPollMs;
    if (slice == 0) break;
    vTaskDelay(pdMS_TO_TICKS(slice));
  }

  SemaphoreHandle_t mu = state_mutex();
  if (mu && xSemaphoreTake(mu, pdMS_TO_TICKS(50)) == pdTRUE) {
    s_retry_delay_ms = 0;
    xSemaphoreGive(mu);
  }
  return !control_request_pending();
}

static bool retry_range_transport(const char* trigger_error)
{
  uint32_t resume_offset = 0;
  uint32_t logical_pos = 0;
  uint32_t cached_bytes = 0;
  SemaphoreHandle_t mu = state_mutex();
  if (!mu || xSemaphoreTake(mu, pdMS_TO_TICKS(100)) != pdTRUE) {
    set_error("flac_prefetch_mutex_failed");
    return false;
  }
  resume_offset = s_network_pos;
  logical_pos = s_logical_pos;
  cached_bytes = static_cast<uint32_t>(s_ring_count);
  xSemaphoreGive(mu);

  char cause[80] = {0};
  if (trigger_error && *trigger_error) {
    strncpy(cause, trigger_error, sizeof(cause) - 1);
  } else {
    strncpy(cause, "http_transport_error", sizeof(cause) - 1);
  }

  if (!error_is_retryable(cause)) {
    set_error(cause);
    if (mu && xSemaphoreTake(mu, pdMS_TO_TICKS(100)) == pdTRUE) {
      s_io_error = true;
      xSemaphoreGive(mu);
    }
    return false;
  }

  LOGW("[NAS FLAC] 网络中断：原因=%s 解码位置=%lu 网络位置=%lu 环形缓存=%luB",
       cause,
       (unsigned long)logical_pos,
       (unsigned long)resume_offset,
       (unsigned long)cached_bytes);

  s_client.stop();
  if (mu && xSemaphoreTake(mu, pdMS_TO_TICKS(100)) == pdTRUE) {
    s_reconnecting = true;
    s_transport_connected = false;
    s_transport_available_bytes = 0;
    s_retry_attempt = 0;
    s_retry_delay_ms = 0;
    xSemaphoreGive(mu);
  }
  publish_snapshot(true);

  for (size_t i = 0; i < kRetryDelayCount; ++i) {
    if (control_request_pending()) {
      if (s_stop_requested || !operation_is_current()) set_error("cancelled");
      if (mu && xSemaphoreTake(mu, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_reconnecting = false;
        s_retry_attempt = 0;
        s_retry_delay_ms = 0;
        xSemaphoreGive(mu);
      }
      publish_snapshot(false);
      return false;
    }

    if (mu && xSemaphoreTake(mu, pdMS_TO_TICKS(100)) == pdTRUE) {
      s_retry_attempt = static_cast<uint8_t>(i + 1);
      s_retry_delay_ms = kRetryDelaysMs[i];
      ++s_reconnect_attempt_count;
      xSemaphoreGive(mu);
    }
    LOGW("[NAS FLAC] 准备 Range 续传：尝试=%u/%u 延迟=%lums offset=%lu 原因=%s",
         (unsigned)(i + 1),
         (unsigned)kRetryDelayCount,
         (unsigned long)kRetryDelaysMs[i],
         (unsigned long)resume_offset,
         cause);
    publish_snapshot(true);

    if (!wait_before_retry(kRetryDelaysMs[i])) {
      if (mu && xSemaphoreTake(mu, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_reconnecting = false;
        s_retry_attempt = 0;
        s_retry_delay_ms = 0;
        xSemaphoreGive(mu);
      }
      publish_snapshot(false);
      return false;
    }

    if (open_range(s_url.c_str(), resume_offset)) {
      if (mu && xSemaphoreTake(mu, pdMS_TO_TICKS(100)) == pdTRUE) {
        ++s_reconnect_success_count;
        s_reconnecting = false;
        s_retry_attempt = 0;
        s_retry_delay_ms = 0;
        s_io_error = false;
        xSemaphoreGive(mu);
      }
      clear_error();
      LOGI("[NAS FLAC] Range 续传成功：offset=%lu 尝试=%u 总成功=%lu Range次数=%lu",
           (unsigned long)resume_offset,
           (unsigned)(i + 1),
           (unsigned long)s_reconnect_success_count,
           (unsigned long)s_range_open_count);
      publish_snapshot(false);
      return true;
    }

    if (s_seek_pending || s_stop_requested || !operation_is_current()) {
      if (mu && xSemaphoreTake(mu, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_reconnecting = false;
        s_retry_attempt = 0;
        s_retry_delay_ms = 0;
        xSemaphoreGive(mu);
      }
      return false;
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

  LOGE("[NAS FLAC] Range 续传失败达到上限：offset=%lu 最后错误=%s",
       (unsigned long)resume_offset,
       cause);
  set_error("flac_reconnect_exhausted");
  if (mu && xSemaphoreTake(mu, pdMS_TO_TICKS(100)) == pdTRUE) {
    s_reconnecting = false;
    s_retry_attempt = 0;
    s_retry_delay_ms = 0;
    s_io_error = true;
    s_transport_connected = false;
    s_transport_available_bytes = 0;
    xSemaphoreGive(mu);
  }
  publish_snapshot(false);
  return false;
}

static bool handle_seek_request()
{
  uint32_t request_id = 0;
  uint32_t offset = 0;
  uint32_t total_size = 0;
  SemaphoreHandle_t mu = state_mutex();
  if (!mu || xSemaphoreTake(mu, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  if (!s_seek_pending) {
    xSemaphoreGive(mu);
    return false;
  }

  request_id = s_seek_request_id;
  offset = s_seek_offset;
  total_size = s_total_size;
  s_seek_pending = false;
  s_reconnecting = false;
  s_retry_attempt = 0;
  s_retry_delay_ms = 0;
  xSemaphoreGive(mu);

  s_client.stop();
  bool ok = false;
  if (offset == total_size) {
    ok = true;
    if (xSemaphoreTake(mu, pdMS_TO_TICKS(100)) == pdTRUE) {
      s_network_pos = offset;
      s_prefetch_eof = true;
      s_transport_connected = false;
      s_transport_available_bytes = 0;
      xSemaphoreGive(mu);
    }
  } else if (!s_stop_requested && operation_is_current()) {
    ok = open_range(s_url.c_str(), offset);
  }

  if (xSemaphoreTake(mu, pdMS_TO_TICKS(100)) == pdTRUE) {
    if (request_id == s_seek_request_id) {
      s_seek_result = ok;
      s_seek_completed_id = request_id;
      s_io_error = !ok;
      if (ok) s_eof = offset >= s_total_size;
    }
    xSemaphoreGive(mu);
  }

  if (!ok && !s_stop_requested && operation_is_current()) {
    LOGW("[NAS FLAC] 跳转 Range 打开失败：offset=%lu 错误=%s",
         (unsigned long)offset,
         s_last_error[0] ? s_last_error : "unknown");
  }
  publish_snapshot(false);
  return true;
}

static void prefetch_task_entry(void*)
{
  LOGI("[NAS FLAC] 预取任务已启动：缓冲=%luB 目标=%luB 栈=%lu 优先级=%u 核心=%d",
       (unsigned long)ring_capacity_bytes(),
       (unsigned long)active_prefetch_target_bytes(),
       (unsigned long)kPrefetchTaskStackBytes,
       (unsigned)kPrefetchTaskPrio,
       (int)kPrefetchTaskCore);

  SemaphoreHandle_t mu = state_mutex();
  for (;;) {
    if (s_stop_requested || !operation_is_current()) break;
    if (handle_seek_request()) continue;

    size_t ring_count = 0;
    size_t ring_free = 0;
    size_t write_index = 0;
    uint32_t generation = 0;
    uint32_t network_pos = 0;
    uint32_t total_size = 0;
    uint32_t last_data_ms = 0;
    uint32_t opened_ms = 0;
    uint32_t prefetch_target = 0;
    const size_t ring_capacity = ring_capacity_bytes();

    if (!mu || xSemaphoreTake(mu, pdMS_TO_TICKS(100)) != pdTRUE) {
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }
    ring_count = s_ring_count;
    ring_free = ring_capacity - s_ring_count;
    write_index = s_ring_write;
    generation = s_ring_generation;
    network_pos = s_network_pos;
    total_size = s_total_size;
    last_data_ms = s_last_body_data_ms;
    opened_ms = s_transport_opened_ms;
    prefetch_target = active_prefetch_target_bytes();
    xSemaphoreGive(mu);

    if (network_pos >= total_size && total_size != 0) {
      s_client.stop();
      if (xSemaphoreTake(mu, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_prefetch_eof = true;
        s_transport_connected = false;
        s_transport_available_bytes = 0;
        xSemaphoreGive(mu);
      }
      publish_snapshot(false);
      vTaskDelay(pdMS_TO_TICKS(3));
      continue;
    }

    if (ring_count >= prefetch_target || ring_free == 0) {
      const int available = s_client.available();
      if (xSemaphoreTake(mu, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_transport_connected = s_client.connected() || available > 0;
        s_transport_available_bytes = available > 0
            ? static_cast<uint32_t>(available)
            : 0;
        xSemaphoreGive(mu);
      }
      publish_snapshot(false);
      // 缓冲目标已达到时等待通知；消费者跌入低水位可立即唤醒，而非固定等 3ms。
      (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(3));
      continue;
    }

    if (!WiFi.isConnected()) {
      set_error("wifi_disconnected");
      if (!retry_range_transport(s_last_error) && !s_seek_pending) break;
      continue;
    }

    const int available = s_client.available();
    if (available > 0) {
      size_t want = static_cast<size_t>(available);
      if (want > kPrefetchReadChunkBytes) want = kPrefetchReadChunkBytes;
      if (want > ring_free) want = ring_free;
      const size_t contiguous = ring_capacity - write_index;
      if (want > contiguous) want = contiguous;
      const size_t file_remaining = static_cast<size_t>(total_size - network_pos);
      if (want > file_remaining) want = file_remaining;

      if (want > 0) {
        const int got = s_client.read(s_ring + write_index, want);
        if (got > 0) {
          if (xSemaphoreTake(mu, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (generation == s_ring_generation && !s_seek_pending) {
              s_ring_write = (s_ring_write + static_cast<size_t>(got)) % ring_capacity;
              s_ring_count += static_cast<size_t>(got);
              s_network_pos += static_cast<uint32_t>(got);
              s_last_body_data_ms = millis();
              s_prefetch_eof = s_network_pos >= s_total_size;
              s_io_error = false;
            }
            const int remaining = s_client.available();
            s_transport_connected = s_client.connected() || remaining > 0;
            s_transport_available_bytes = remaining > 0
                ? static_cast<uint32_t>(remaining)
                : 0;
            xSemaphoreGive(mu);
          }
          publish_snapshot(false);
          continue;
        }
      }
    }

    const bool connected = s_client.connected() || s_client.available() > 0;
    if (xSemaphoreTake(mu, pdMS_TO_TICKS(100)) == pdTRUE) {
      s_transport_connected = connected;
      const int pending = s_client.available();
      s_transport_available_bytes = pending > 0
          ? static_cast<uint32_t>(pending)
          : 0;
      xSemaphoreGive(mu);
    }

    if (!connected) {
      if (network_pos >= total_size && total_size != 0) {
        if (xSemaphoreTake(mu, pdMS_TO_TICKS(100)) == pdTRUE) {
          s_prefetch_eof = true;
          xSemaphoreGive(mu);
        }
        continue;
      }
      set_error("http_range_disconnected_early");
      if (!retry_range_transport(s_last_error) && !s_seek_pending) break;
      continue;
    }

    const uint32_t activity_base = last_data_ms ? last_data_ms : opened_ms;
    if (activity_base != 0 &&
        (uint32_t)(millis() - activity_base) >= kNoDataTimeoutMs) {
      set_error("stream_no_data_timeout");
      if (!retry_range_transport(s_last_error) && !s_seek_pending) break;
      continue;
    }

    publish_snapshot(false);
    vTaskDelay(pdMS_TO_TICKS(1));
  }

  s_client.stop();
  if (mu && xSemaphoreTake(mu, pdMS_TO_TICKS(100)) == pdTRUE) {
    s_prefetch_running = false;
    s_transport_connected = false;
    s_transport_available_bytes = 0;
    s_reconnecting = false;
    s_retry_attempt = 0;
    s_retry_delay_ms = 0;
    s_prefetch_task = nullptr;
    xSemaphoreGive(mu);
  } else {
    s_prefetch_task = nullptr;
  }
  publish_snapshot(false);
  LOGD("[NAS FLAC] 预取任务已退出");
  vTaskDelete(nullptr);
}

static TaskHandle_t prefetch_task_handle()
{
  TaskHandle_t handle = nullptr;
  SemaphoreHandle_t mu = state_mutex();
  if (mu && xSemaphoreTake(mu, pdMS_TO_TICKS(50)) == pdTRUE) {
    handle = s_prefetch_task;
    xSemaphoreGive(mu);
  }
  return handle;
}

static bool start_prefetch_task()
{
  SemaphoreHandle_t mu = state_mutex();
  if (!mu || xSemaphoreTake(mu, pdMS_TO_TICKS(100)) != pdTRUE) {
    set_error("flac_prefetch_mutex_failed");
    return false;
  }
  s_stop_requested = false;
  s_prefetch_running = true;
  xSemaphoreGive(mu);

  const BaseType_t ok = xTaskCreatePinnedToCore(prefetch_task_entry,
                                                "FlacNetTask",
                                                kPrefetchTaskStackBytes,
                                                nullptr,
                                                kPrefetchTaskPrio,
                                                &s_prefetch_task,
                                                kPrefetchTaskCore);
  if (ok != pdPASS || !s_prefetch_task) {
    if (xSemaphoreTake(mu, pdMS_TO_TICKS(100)) == pdTRUE) {
      s_prefetch_running = false;
      xSemaphoreGive(mu);
    }
    set_error("flac_prefetch_task_create_failed");
    LOGE("[NAS FLAC] 创建预取任务失败：返回值=%ld", (long)ok);
    return false;
  }
  return true;
}

}  // namespace

bool audio_http_range_source_open(const char* url, uint32_t operation_id)
{
  audio_http_range_source_close();
  clear_error();

  if (prefetch_task_handle()) {
    set_error("flac_prefetch_stop_timeout");
    return false;
  }
  if (!state_mutex()) {
    set_error("flac_prefetch_mutex_failed");
    return false;
  }
  if (!ensure_ring()) return false;

  SemaphoreHandle_t mu = state_mutex();
  if (xSemaphoreTake(mu, pdMS_TO_TICKS(100)) != pdTRUE) {
    set_error("flac_prefetch_mutex_failed");
    return false;
  }
  s_operation_id = operation_id;
  s_open = true;
  s_io_error = false;
  s_eof = false;
  s_prefetch_eof = false;
  s_reconnecting = false;
  s_transport_connected = false;
  s_transport_available_bytes = 0;
  s_retry_attempt = 0;
  s_retry_delay_ms = 0;
  s_reconnect_attempt_count = 0;
  s_reconnect_success_count = 0;
  s_transport_opened_ms = 0;
  s_last_body_data_ms = 0;
  s_waiting_since_ms = 0;
  s_last_wait_warning_ms = 0;
  s_last_buffer_diag_ms = 0;
  s_diag_ring_peak_bytes = 0;
  s_diag_ring_empty_count = 0;
  s_diag_ring_empty_start_ms = 0;
  s_diag_ring_empty_total_ms = 0;
  s_diag_reader_wait_count = 0;
  s_diag_reader_wait_total_us = 0;
  s_diag_low_watermark_count = 0;
  s_diag_min_cached_bytes = 0;
  s_diag_low_water_active = false;
  s_diag_playback_tracking_started = false;
  s_prefetch_target_bytes = clamp_prefetch_target(kDefaultPrefetchTargetBytes);
  s_total_size = 0;
  s_network_pos = 0;
  s_range_open_count = 0;
  s_seek_pending = false;
  s_seek_offset = 0;
  s_seek_request_id = 0;
  s_seek_completed_id = 0;
  s_seek_result = false;
  reset_ring_locked(0);
  xSemaphoreGive(mu);

  if (!operation_is_current()) {
    set_error("cancelled");
    audio_http_range_source_close();
    return false;
  }
  if (!WiFi.isConnected()) {
    set_error("wifi_disconnected");
    audio_http_range_source_close();
    return false;
  }

  if (!open_range(url, 0)) {
    if (xSemaphoreTake(mu, pdMS_TO_TICKS(100)) == pdTRUE) {
      s_io_error = true;
      xSemaphoreGive(mu);
    }
    audio_http_range_source_close();
    return false;
  }

  if (!start_prefetch_task()) {
    audio_http_range_source_close();
    return false;
  }

  uint32_t total_size = 0;
  if (xSemaphoreTake(mu, pdMS_TO_TICKS(100)) != pdTRUE) {
    set_error("flac_prefetch_mutex_failed");
    audio_http_range_source_close();
    return false;
  }
  total_size = s_total_size;
  xSemaphoreGive(mu);
  const uint32_t target = total_size < kStartupPrefillBytes
      ? total_size
      : kStartupPrefillBytes;
  const uint32_t wait_started_ms = millis();
  uint32_t cached = 0;
  bool prefetch_eof = false;
  bool io_error = false;

  for (;;) {
    if (xSemaphoreTake(mu, pdMS_TO_TICKS(100)) != pdTRUE) {
      set_error("flac_prefetch_mutex_failed");
      audio_http_range_source_close();
      return false;
    }
    cached = static_cast<uint32_t>(s_ring_count);
    prefetch_eof = s_prefetch_eof;
    io_error = s_io_error;
    xSemaphoreGive(mu);
    if (cached >= target || prefetch_eof || io_error) break;
    if (!operation_is_current()) {
      set_error("cancelled");
      audio_http_range_source_close();
      return false;
    }
    if ((uint32_t)(millis() - wait_started_ms) >= kStartupPrefillTimeoutMs) break;
    publish_snapshot(true);
    vTaskDelay(pdMS_TO_TICKS(2));
  }

  if (io_error || (cached == 0 && !prefetch_eof)) {
    if (!s_last_error[0]) set_error("flac_prefetch_startup_failed");
    LOGE("[NAS FLAC] 启动预取失败：缓存=%luB 目标=%luB 错误=%s",
         (unsigned long)cached,
         (unsigned long)target,
         s_last_error);
    audio_http_range_source_close();
    return false;
  }

  if (cached < target && cached < kStartupMinimumBytes && !prefetch_eof) {
    LOGW("[NAS FLAC] 启动预取未达到最低建议值：缓存=%luB 目标=%luB，继续起播",
         (unsigned long)cached,
         (unsigned long)target);
  }

  LOGI("[NAS FLAC] HTTP Range 音源打开成功：size=%lu 启动缓存=%lu/%luB URL=%s",
       (unsigned long)total_size,
       (unsigned long)cached,
       (unsigned long)ring_capacity_bytes(),
       s_url.c_str());
  publish_snapshot(false);
  return true;
}

void audio_http_range_source_close()
{
  SemaphoreHandle_t mu = state_mutex();
  bool was_open = false;
  uint32_t range_count = 0;
  uint32_t reconnect_attempts = 0;
  uint32_t reconnect_successes = 0;
  uint32_t logical_pos = 0;
  uint32_t total_size = 0;

  if (mu && xSemaphoreTake(mu, pdMS_TO_TICKS(100)) == pdTRUE) {
    was_open = s_open;
    range_count = s_range_open_count;
    reconnect_attempts = s_reconnect_attempt_count;
    reconnect_successes = s_reconnect_success_count;
    logical_pos = s_logical_pos;
    total_size = s_total_size;
    s_stop_requested = true;
    xSemaphoreGive(mu);
  } else {
    s_stop_requested = true;
  }

  const uint32_t wait_started_ms = millis();
  while (prefetch_task_handle() &&
         (uint32_t)(millis() - wait_started_ms) < kStopTimeoutMs) {
    vTaskDelay(pdMS_TO_TICKS(2));
  }

  if (prefetch_task_handle()) {
    set_error("flac_prefetch_stop_timeout");
    if (mu && xSemaphoreTake(mu, pdMS_TO_TICKS(100)) == pdTRUE) {
      s_io_error = true;
      xSemaphoreGive(mu);
    }
    LOGE("[NAS FLAC] 预取任务停止超时，为避免跨任务操作 WiFiClient 暂不强制删除");
    publish_snapshot(false);
    return;
  }

  if (was_open && (reconnect_attempts > 0 || range_count > 1)) {
    LOGI("[NAS FLAC] Range 音源关闭统计：Range=%lu 续传尝试=%lu 成功=%lu 最终位置=%lu/%lu",
         (unsigned long)range_count,
         (unsigned long)reconnect_attempts,
         (unsigned long)reconnect_successes,
         (unsigned long)logical_pos,
         (unsigned long)total_size);
  }

  s_client.stop();
  s_url = String();

  if (mu && xSemaphoreTake(mu, pdMS_TO_TICKS(100)) == pdTRUE) {
    s_operation_id = 0;
    s_logical_pos = 0;
    s_network_pos = 0;
    s_total_size = 0;
    s_range_open_count = 0;
    s_open = false;
    s_io_error = false;
    s_eof = false;
    s_prefetch_eof = false;
    s_reconnecting = false;
    s_transport_connected = false;
    s_transport_available_bytes = 0;
    s_retry_attempt = 0;
    s_retry_delay_ms = 0;
    s_reconnect_attempt_count = 0;
    s_reconnect_success_count = 0;
    s_transport_opened_ms = 0;
    s_last_body_data_ms = 0;
    s_waiting_since_ms = 0;
    s_last_wait_warning_ms = 0;
    s_last_buffer_diag_ms = 0;
    s_stop_requested = false;
    s_seek_pending = false;
    s_seek_offset = 0;
    s_seek_request_id = 0;
    s_seek_completed_id = 0;
    s_seek_result = false;
    reset_ring_locked(0);
    xSemaphoreGive(mu);
  }

  // PSRAM 环形缓冲首次分配后常驻复用，避免连续切歌造成内存碎片。
  publish_snapshot(false);
}

ssize_t audio_http_range_source_read(void* dst, size_t bytes)
{
  if (!dst || bytes == 0) return 0;
  SemaphoreHandle_t mu = state_mutex();
  if (!mu) {
    set_error("flac_prefetch_mutex_failed");
    return -1;
  }

  uint8_t* output = static_cast<uint8_t*>(dst);
  size_t copied = 0;
#if APP_DIAG_NAS_FLAC_PERFORMANCE
  int64_t wait_started_us = 0;
#endif

  while (copied < bytes) {
    bool open = false;
    bool io_error = false;
    bool prefetch_eof = false;
    bool eof = false;
    size_t copied_now = 0;
#if APP_DIAG_NAS_FLAC_PERFORMANCE
    bool recorded_wait = false;
#endif

    if (xSemaphoreTake(mu, pdMS_TO_TICKS(100)) != pdTRUE) {
      set_error("flac_prefetch_mutex_failed");
      return copied > 0 ? static_cast<ssize_t>(copied) : -1;
    }

    open = s_open;
    io_error = s_io_error;
    prefetch_eof = s_prefetch_eof;
    eof = s_eof;

    update_low_watermark_locked(s_ring_count);
    if (open && s_ring_count > 0) {
      size_t want = bytes - copied;
      if (want > kPrefetchReadChunkBytes) want = kPrefetchReadChunkBytes;
      if (want > s_ring_count) want = s_ring_count;
      const size_t contiguous = ring_capacity_bytes() - s_ring_read;
      if (want > contiguous) want = contiguous;

      memcpy(output + copied, s_ring + s_ring_read, want);
      s_ring_read = (s_ring_read + want) % ring_capacity_bytes();
      s_ring_count -= want;
      s_logical_pos += static_cast<uint32_t>(want);
      s_eof = s_total_size != 0 && s_logical_pos >= s_total_size;
      copied_now = want;
      update_low_watermark_locked(s_ring_count);

#if APP_DIAG_NAS_FLAC_PERFORMANCE
      if (wait_started_us != 0) {
        const int64_t now_us = esp_timer_get_time();
        if (now_us > wait_started_us) {
          record_reader_wait_locked(static_cast<uint64_t>(now_us - wait_started_us));
          recorded_wait = true;
        }
        wait_started_us = 0;
      }
#endif
      if (s_ring_count < kLowWatermarkBytes && s_prefetch_task) {
        xTaskNotifyGive(s_prefetch_task);
      }
    } else if (open && !prefetch_eof && !io_error && s_prefetch_task) {
      xTaskNotifyGive(s_prefetch_task);
    }
    xSemaphoreGive(mu);
#if APP_DIAG_NAS_FLAC_PERFORMANCE
    if (recorded_wait) publish_snapshot(false);
#endif

    if (!open) {
      set_error("source_not_open");
      return copied > 0 ? static_cast<ssize_t>(copied) : -1;
    }
    if (copied_now > 0) {
      copied += copied_now;
      continue;
    }
    if (eof || prefetch_eof) {
      publish_snapshot(false);
      return static_cast<ssize_t>(copied);
    }
    if (io_error) {
      return copied > 0 ? static_cast<ssize_t>(copied) : -1;
    }
    if (!operation_is_current()) {
      set_error("cancelled");
      return copied > 0 ? static_cast<ssize_t>(copied) : -1;
    }

#if APP_DIAG_NAS_FLAC_PERFORMANCE
    if (wait_started_us == 0) {
      wait_started_us = esp_timer_get_time();
    }
#endif
    publish_snapshot(true);
    vTaskDelay(pdMS_TO_TICKS(1));
  }

  publish_snapshot(false);
  return static_cast<ssize_t>(copied);
}

bool audio_http_range_source_seek(uint32_t absolute_offset)
{
  SemaphoreHandle_t mu = state_mutex();
  if (!mu || xSemaphoreTake(mu, pdMS_TO_TICKS(100)) != pdTRUE) {
    set_error("flac_prefetch_mutex_failed");
    return false;
  }

  if (!s_open || absolute_offset > s_total_size) {
    set_error(!s_open ? "source_not_open" : "seek_out_of_range");
    xSemaphoreGive(mu);
    return false;
  }
  if (absolute_offset == s_logical_pos) {
    xSemaphoreGive(mu);
    return true;
  }

  // 向前跳转如果仍落在未消费环形缓冲中，只丢弃前面的字节，不重连 NAS。
  if (absolute_offset > s_logical_pos) {
    const uint32_t delta = absolute_offset - s_logical_pos;
    if (delta <= s_ring_count) {
      s_ring_read = (s_ring_read + static_cast<size_t>(delta)) % ring_capacity_bytes();
      s_ring_count -= static_cast<size_t>(delta);
      s_logical_pos = absolute_offset;
      s_eof = absolute_offset >= s_total_size;
      xSemaphoreGive(mu);
      publish_snapshot(false);
      return true;
    }
  }

  ++s_seek_request_id;
  if (s_seek_request_id == 0) s_seek_request_id = 1;
  const uint32_t request_id = s_seek_request_id;
  s_seek_offset = absolute_offset;
  s_seek_pending = true;
  s_seek_result = false;
  s_io_error = false;
  s_prefetch_eof = false;
  s_transport_connected = false;
  s_transport_available_bytes = 0;
  s_network_pos = absolute_offset;
  reset_ring_locked(absolute_offset);
  xSemaphoreGive(mu);

  const uint32_t started_ms = millis();
  for (;;) {
    bool completed = false;
    bool result = false;
    bool running = false;
    if (xSemaphoreTake(mu, pdMS_TO_TICKS(100)) == pdTRUE) {
      completed = s_seek_completed_id == request_id;
      result = s_seek_result;
      running = s_prefetch_running;
      xSemaphoreGive(mu);
    }
    if (completed) {
      publish_snapshot(false);
      return result;
    }
    if (!running || !operation_is_current()) {
      set_error("cancelled");
      return false;
    }
    if ((uint32_t)(millis() - started_ms) >= kSeekTimeoutMs) {
      set_error("flac_range_seek_timeout");
      if (xSemaphoreTake(mu, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_io_error = true;
        xSemaphoreGive(mu);
      }
      publish_snapshot(false);
      return false;
    }
    publish_snapshot(true);
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

uint32_t audio_http_range_source_tell()
{
  uint32_t value = 0;
  SemaphoreHandle_t mu = state_mutex();
  if (mu && xSemaphoreTake(mu, pdMS_TO_TICKS(50)) == pdTRUE) {
    value = s_logical_pos;
    xSemaphoreGive(mu);
  }
  return value;
}

uint32_t audio_http_range_source_size()
{
  uint32_t value = 0;
  SemaphoreHandle_t mu = state_mutex();
  if (mu && xSemaphoreTake(mu, pdMS_TO_TICKS(50)) == pdTRUE) {
    value = s_total_size;
    xSemaphoreGive(mu);
  }
  return value;
}

bool audio_http_range_source_had_io_error()
{
  bool value = true;
  SemaphoreHandle_t mu = state_mutex();
  if (mu && xSemaphoreTake(mu, pdMS_TO_TICKS(50)) == pdTRUE) {
    value = s_io_error;
    xSemaphoreGive(mu);
  }
  return value;
}

bool audio_http_range_source_is_open()
{
  bool value = false;
  SemaphoreHandle_t mu = state_mutex();
  if (mu && xSemaphoreTake(mu, pdMS_TO_TICKS(50)) == pdTRUE) {
    value = s_open;
    xSemaphoreGive(mu);
  }
  return value;
}

const char* audio_http_range_source_get_last_error()
{
  return s_last_error;
}

void audio_http_range_source_set_flac_profile(uint32_t average_bitrate_kbps,
                                               uint32_t sample_rate,
                                               uint8_t bits_per_sample)
{
  SemaphoreHandle_t mu = state_mutex();
  if (!mu) return;

  const bool high_rate = average_bitrate_kbps >= 2500 ||
                         sample_rate >= 88200 ||
                         bits_per_sample >= 24;
  uint32_t target = 0;
  uint32_t capacity = 0;
  if (xSemaphoreTake(mu, pdMS_TO_TICKS(100)) != pdTRUE) return;
  s_prefetch_target_bytes = clamp_prefetch_target(
      high_rate ? kHighRatePrefetchTargetBytes : kDefaultPrefetchTargetBytes);
  // 真实播放期统计在软件预填充完成后启动，避免解码器打开阶段的零缓存污染最低值。
  s_diag_reader_wait_count = 0;
  s_diag_reader_wait_total_us = 0;
  s_diag_low_watermark_count = 0;
  s_diag_min_cached_bytes = 0;
  s_diag_low_water_active = false;
  s_diag_playback_tracking_started = false;
  target = active_prefetch_target_bytes();
  capacity = ring_capacity_bytes();
  if (s_prefetch_task) xTaskNotifyGive(s_prefetch_task);
  xSemaphoreGive(mu);
  publish_snapshot(false);

  LOGI("[NAS FLAC] 预取规格已更新：码率=%luKbps 采样率=%luHz 位深=%u 高负载=%d 目标=%lu/%luB",
       (unsigned long)average_bitrate_kbps,
       (unsigned long)sample_rate,
       (unsigned)bits_per_sample,
       high_rate ? 1 : 0,
       (unsigned long)target,
       (unsigned long)capacity);
}

void audio_http_range_source_reset_playback_diagnostics()
{
#if APP_DIAG_NAS_FLAC_PERFORMANCE
  SemaphoreHandle_t mu = state_mutex();
  if (!mu || xSemaphoreTake(mu, pdMS_TO_TICKS(100)) != pdTRUE) return;

  s_diag_reader_wait_count = 0;
  s_diag_reader_wait_total_us = 0;
  s_diag_low_watermark_count = 0;
  s_diag_min_cached_bytes = s_ring_count;
  s_diag_low_water_active = s_ring_count < kLowWatermarkBytes;
  s_diag_playback_tracking_started = true;
  xSemaphoreGive(mu);
  publish_snapshot(false);
#endif
}

bool audio_http_range_source_get_snapshot(AudioHttpRangeSourceSnapshot* out_snapshot)
{
  if (!out_snapshot) return false;
  portENTER_CRITICAL(&s_snapshot_mux);
  *out_snapshot = s_snapshot;
  portEXIT_CRITICAL(&s_snapshot_mux);
  return true;
}
