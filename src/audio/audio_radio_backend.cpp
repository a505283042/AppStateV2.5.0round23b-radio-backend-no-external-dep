#include "audio/audio_radio_backend.h"

#include <stdlib.h>
#include <string.h>

#include "audio/audio_service.h"
#include "utils/log.h"

namespace {
RadioBackendStatus s_status{};
String s_station;
String s_url;
String s_region;
bool s_inited = false;
bool s_session_active = false;
bool s_retry_waiting = false;
uint32_t s_pending_request_id = 0;
uint32_t s_pending_started_ms = 0;
uint32_t s_retry_due_ms = 0;
uint32_t s_playing_since_ms = 0;
uint8_t s_retry_count = 0;

static constexpr uint32_t kRadioStartTimeoutMs = 25000;
static constexpr uint32_t kRetryStableResetMs = 30000;
static constexpr uint32_t kRetryDelayMs[] = {1000, 2000, 4000, 8000, 15000};
static constexpr uint8_t kRetryLimit =
    (uint8_t)(sizeof(kRetryDelayMs) / sizeof(kRetryDelayMs[0]));

static bool time_reached(uint32_t now_ms, uint32_t target_ms)
{
  return (int32_t)(now_ms - target_ms) >= 0;
}

static bool error_is(const char* error, const char* expected)
{
  return error && expected && strcmp(error, expected) == 0;
}

static bool http_status_is_retryable(const char* error)
{
  static constexpr const char* kPrefix = "http_status_";
  if (!error || strncmp(error, kPrefix, strlen(kPrefix)) != 0) {
    return false;
  }

  const int status = atoi(error + strlen(kPrefix));
  return status == 408 || status == 425 || status == 429 ||
         (status >= 500 && status <= 599);
}

static bool error_is_retryable(const char* error)
{
  if (!error || !*error) return true;
  if (http_status_is_retryable(error)) return true;

  return error_is(error, "queue_failed") ||
         error_is(error, "startup_timeout") ||
         error_is(error, "wifi_disconnected") ||
         error_is(error, "http_connect_failed") ||
         error_is(error, "http_header_timeout") ||
         error_is(error, "http_header_disconnected") ||
         error_is(error, "http_header_incomplete") ||
         error_is(error, "http_status_line_missing") ||
         error_is(error, "stream_start_no_data") ||
         error_is(error, "stream_prefill_failed") ||
         error_is(error, "stream_ended_before_audio") ||
         error_is(error, "stream_no_data_timeout") ||
         error_is(error, "stream_read_failed") ||
         error_is(error, "stream_interrupted") ||
         error_is(error, "stream_ended") ||
         error_is(error, "stream_start_failed");
}

static void restore_station_fields()
{
  s_status.supported = true;
  s_status.station = s_station;
  s_status.info = s_region;
  s_status.retry_limit = kRetryLimit;
}

static void set_connecting_status(bool retrying, uint32_t retry_in_ms = 0)
{
  restore_station_fields();
  s_status.active = true;
  s_status.paused = false;
  s_status.connecting = true;
  s_status.running = false;
  s_status.retrying = retrying;
  s_status.retry_attempt = retrying ? s_retry_count : 0;
  s_status.retry_in_ms = retry_in_ms;
  s_status.bitrate = 0;
  s_status.sample_rate = 0;
  s_status.channels = 0;
  s_status.inbuf_filled = 0;
  s_status.inbuf_size = 0;
  s_status.error.remove(0);
}

static void set_terminal_error(const char* error)
{
  s_session_active = false;
  s_retry_waiting = false;
  s_pending_request_id = 0;
  s_pending_started_ms = 0;
  s_retry_due_ms = 0;
  s_playing_since_ms = 0;

  restore_station_fields();
  s_status.active = false;
  s_status.paused = false;
  s_status.connecting = false;
  s_status.running = false;
  s_status.retrying = false;
  s_status.retry_attempt = s_retry_count;
  s_status.retry_in_ms = 0;
  s_status.bitrate = 0;
  s_status.sample_rate = 0;
  s_status.channels = 0;
  s_status.inbuf_filled = 0;
  s_status.inbuf_size = 0;
  s_status.error = (error && *error) ? String(error) : String("stream_failed");
}

static bool queue_current_station(bool retrying)
{
  if (!s_session_active || s_url.length() == 0) {
    return false;
  }

  uint32_t request_id = 0;
  const bool queued = audio_service_play_stream_mp3_async(s_url.c_str(), &request_id);
  if (!queued || request_id == 0) {
    return false;
  }

  s_pending_request_id = request_id;
  s_pending_started_ms = millis();
  set_connecting_status(retrying);

  if (retrying) {
    LOGI("[电台] 自动重连请求已入队：请求=%lu 次数=%u/%u 名称=%s",
         (unsigned long)s_pending_request_id,
         (unsigned)s_retry_count,
         (unsigned)kRetryLimit,
         s_station.c_str());
  } else {
    LOGI("[电台] 异步起播请求已入队：请求=%lu 名称=%s",
         (unsigned long)s_pending_request_id,
         s_station.c_str());
  }
  return true;
}

static void schedule_retry_or_fail(const char* error)
{
  const char* effective_error = (error && *error) ? error : "stream_interrupted";
  s_pending_request_id = 0;
  s_pending_started_ms = 0;
  s_playing_since_ms = 0;

  if (!s_session_active) {
    return;
  }

  if (!error_is_retryable(effective_error)) {
    LOGW("[电台] 错误不可重试，停止自动重连：原因=%s", effective_error);
    set_terminal_error(effective_error);
    return;
  }

  if (s_retry_count >= kRetryLimit) {
    LOGW("[电台] 自动重连次数已耗尽：次数=%u 原因=%s",
         (unsigned)s_retry_count,
         effective_error);
    set_terminal_error(effective_error);
    return;
  }

  const uint32_t delay_ms = kRetryDelayMs[s_retry_count];
  ++s_retry_count;
  s_retry_waiting = true;
  s_retry_due_ms = millis() + delay_ms;
  set_connecting_status(true, delay_ms);

  LOGW("[电台] 播放中断，%lums 后自动重连：次数=%u/%u 原因=%s",
       (unsigned long)delay_ms,
       (unsigned)s_retry_count,
       (unsigned)kRetryLimit,
       effective_error);
}

static void update_running_status(const AudioNetworkStateSnapshot& network, bool paused)
{
  restore_station_fields();
  s_status.active = true;
  s_status.paused = paused;
  s_status.connecting = !paused &&
                        network.source_open &&
                        network.transport_connected &&
                        network.waiting_for_data;
  s_status.running = !paused &&
                     network.source_open &&
                     network.transport_connected &&
                     !network.eof;
  s_status.retrying = false;
  s_status.retry_attempt = s_retry_count;
  s_status.retry_in_ms = 0;
  s_status.bitrate = network.bitrate_kbps;
  s_status.sample_rate = network.sample_rate;
  s_status.channels = network.channels;
  s_status.inbuf_filled = network.available_bytes;
  s_status.inbuf_size = 0;
  s_status.error.remove(0);
}

}  // namespace

bool audio_radio_backend_begin()
{
  if (s_inited) return true;
  s_inited = true;
  return true;
}

bool audio_radio_backend_start(const RadioItem& item)
{
  audio_radio_backend_begin();

  s_station = item.name;
  s_url = item.url;
  s_region = item.region;
  s_status = RadioBackendStatus{};
  s_session_active = true;
  s_retry_waiting = false;
  s_pending_request_id = 0;
  s_pending_started_ms = 0;
  s_retry_due_ms = 0;
  s_playing_since_ms = 0;
  s_retry_count = 0;

  restore_station_fields();
  set_connecting_status(false);

  if (s_url.length() == 0) {
    set_terminal_error("invalid_url");
    return false;
  }

  if (queue_current_station(false)) {
    return true;
  }

  schedule_retry_or_fail("queue_failed");
  return s_session_active;
}

void audio_radio_backend_stop()
{
  // 先关闭逻辑会话，再取消 AudioTask；这样取消快照返回时不会再次触发自动重连。
  s_session_active = false;
  s_retry_waiting = false;
  s_pending_request_id = 0;
  s_pending_started_ms = 0;
  s_retry_due_ms = 0;
  s_playing_since_ms = 0;
  s_retry_count = 0;

  audio_service_stop(true);
  s_status = RadioBackendStatus{};
  s_status.supported = true;
  s_status.retry_limit = kRetryLimit;
}

void audio_radio_backend_loop()
{
  if (!s_session_active) {
    return;
  }

  const uint32_t now_ms = millis();

  if (s_retry_waiting) {
    if (!time_reached(now_ms, s_retry_due_ms)) {
      const uint32_t remaining_ms = s_retry_due_ms - now_ms;
      set_connecting_status(true, remaining_ms);
      return;
    }

    s_retry_waiting = false;
    s_retry_due_ms = 0;
    if (!queue_current_station(true)) {
      schedule_retry_or_fail("queue_failed");
    }
    return;
  }

  AudioNetworkStateSnapshot network{};
  (void)audio_service_get_network_state(&network);

  const bool paused = audio_service_is_paused();
  s_status.supported = true;

  if (s_pending_request_id != 0) {
    if ((uint32_t)(now_ms - s_pending_started_ms) >= kRadioStartTimeoutMs) {
      const uint32_t timed_out_request = s_pending_request_id;
      s_pending_request_id = 0;
      s_pending_started_ms = 0;

      // stop(false) 会立即使当前网络 operation 失效，AudioTask 随后完成资源回收。
      (void)audio_service_stop(false);
      LOGW("[电台] 异步起播超时：请求=%lu", (unsigned long)timed_out_request);
      schedule_retry_or_fail("startup_timeout");
      return;
    }

    // 请求尚未被 AudioTask 取出时，快照仍可能是上一条请求；保持连接中。
    if (network.start_request_id != s_pending_request_id) {
      set_connecting_status(s_retry_count > 0);
      return;
    }

    if (network.start_phase == AudioNetworkStartPhase::Connecting) {
      set_connecting_status(s_retry_count > 0);
      return;
    }

    if (network.start_phase == AudioNetworkStartPhase::Failed ||
        network.start_phase == AudioNetworkStartPhase::Cancelled) {
      const uint32_t failed_request = s_pending_request_id;
      const String error = network.start_phase == AudioNetworkStartPhase::Cancelled
          ? String("stream_interrupted")
          : (network.error[0] ? String(network.error) : String("stream_start_failed"));

      s_pending_request_id = 0;
      s_pending_started_ms = 0;
      LOGW("[电台] 异步起播失败：请求=%lu 原因=%s",
           (unsigned long)failed_request,
           error.c_str());
      schedule_retry_or_fail(error.c_str());
      return;
    }

    if (network.start_phase == AudioNetworkStartPhase::Playing) {
      if (network.active) {
        LOGI("[电台] 异步起播完成：请求=%lu 重试次数=%u",
             (unsigned long)s_pending_request_id,
             (unsigned)s_retry_count);
        s_pending_request_id = 0;
        s_pending_started_ms = 0;
        s_playing_since_ms = now_ms;
        update_running_status(network, paused);
        return;
      }

      // 起播成功快照可能只维持很短时间；如果首次轮询时流已经断开，
      // 直接按运行期断流处理，不能继续伪装成连接中直到 25 秒超时。
      const String error = network.error[0]
          ? String(network.error)
          : (network.eof ? String("stream_ended") : String("stream_interrupted"));
      s_pending_request_id = 0;
      s_pending_started_ms = 0;
      schedule_retry_or_fail(error.c_str());
      return;
    }

    set_connecting_status(s_retry_count > 0);
    return;
  }

  if (network.active) {
    if (s_playing_since_ms == 0) {
      s_playing_since_ms = now_ms;
    }

    if (s_retry_count > 0 &&
        (uint32_t)(now_ms - s_playing_since_ms) >= kRetryStableResetMs) {
      LOGI("[电台] 重连后已稳定播放 %lums，重置重连计数",
           (unsigned long)kRetryStableResetMs);
      s_retry_count = 0;
    }

    update_running_status(network, paused);
    return;
  }

  // 网络电台属于连续流，只要会话仍有效，任何 EOF、断流或读取失败都不是正常结束。
  const char* runtime_error = network.error[0]
      ? network.error
      : (network.eof ? "stream_ended" : "stream_interrupted");
  schedule_retry_or_fail(runtime_error);
}

bool audio_radio_backend_is_active()
{
  return s_status.active;
}

bool audio_radio_backend_is_paused()
{
  return audio_radio_backend_is_active() && audio_service_is_paused();
}

bool audio_radio_backend_toggle_pause()
{
  // 正在等待重连或尚未完成起播时没有可暂停的解码器。
  if (!s_status.running && !s_status.paused) return false;
  return audio_service_is_paused()
      ? audio_service_resume(true)
      : audio_service_pause(true);
}

RadioBackendStatus audio_radio_backend_get_status()
{
  return s_status;
}

const char* audio_radio_backend_name()
{
  return "audiotools-urlstream";
}
