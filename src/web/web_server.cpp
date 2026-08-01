#include "web/web_server.h"

#include <WiFi.h>
#include <WebServer.h>
#include <SdFat.h>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <esp32-hal-psram.h>

#include "app_state.h"
#include "app_power.h"
#include "app_alarm.h"
#include "app_flags.h"
#include "audio/audio_service.h"
#include "audio/audio.h"
#include "audio/audio_output_route.h"
#include "player_control.h"
#include "player_snapshot.h"
#include "player_state.h"
#include "player_source.h"
#include "lyrics/lyrics.h"
#include "nfc/nfc_binding.h"
#include "nfc/nfc_binding_commit.h"
#include "player_binding.h"
#include "player_recover.h"
#include "radio/radio_catalog.h"
#include "net_music/net_music_catalog.h"
#include "net_music/net_music_embedded_cover.h"
#include "player_list_select.h"
#include "player_playlist.h"
#include "storage/storage_catalog_v3.h"
#include "storage/storage_view_v3.h"
#include "storage/storage_io.h"
#include "storage/storage.h"
#include "storage/storage_config_writer.h"
#include "storage/storage_groups_v3.h"
#include "storage/system_paths.h"
#include "ui/ui.h"
#include "menu/quick_menu.h"
#include "utils/log.h"
#include "web/web_config.h"
#include "web/web_page.h"
#include "web/web_snapshot.h"
#include "web/web_settings.h"
#include "web/web_cover_cache.h"
#include "hal/pcf85063.h"
#include "hal/board_hw_control.h"
#include "hal/bq27441.h"
#include "hal/i2c_bus_lock.h"
#include "hal/mcp23017_u3.h"
#include "hal/bluetooth_restart_controller.h"
#include "hal/bt62sp_uart_debug.h"
#include "hal/ws2812_status.h"

extern SdFat sd;

struct WebWifiNetwork {
  String ssid;
  String password;
  bool hidden = false;
  int channel = 0;
  bool has_bssid = false;
  uint8_t bssid[6] = {0};
  String bssid_text;
};

static constexpr size_t kWebWifiMaxNetworks = 16;
static constexpr size_t kWebWifiMaxSsidBytes = 32;
static constexpr size_t kWebWifiMaxPasswordBytes = 64;
static constexpr size_t kWebWifiMaxHostnameBytes = 63;

struct WebRadioConfigItem {
  String name;
  String url;
  String format;
  String region;
  String logo;
};

struct WebRadioParseStats {
  size_t data_lines = 0;
  size_t skipped_lines = 0;
  size_t first_skipped_line = 0;
  String first_skip_reason;
};

static constexpr size_t kWebRadioMaxItems = 64;
static constexpr size_t kWebRadioMaxNameBytes = 96;
static constexpr size_t kWebRadioMaxUrlBytes = 512;
static constexpr size_t kWebRadioMaxFormatBytes = 24;
static constexpr size_t kWebRadioMaxRegionBytes = 64;
static constexpr size_t kWebRadioMaxLogoBytes = 192;
static constexpr size_t kWebRadioConfigMaxBytes = 48 * 1024;
static constexpr size_t kWebRadioLogoMaxBytes = 1024 * 1024;
static constexpr size_t kWebSystemAssetMaxBytes = 400 * 1024;

struct WebNasConfigSource {
  String name;
  String relative_path;
  String list_name;
};

static constexpr size_t kWebNasMaxSources = 8;
static constexpr size_t kWebNasMaxBaseUrlBytes = 768;
static constexpr size_t kWebNasMaxNameBytes = 96;
static constexpr size_t kWebNasMaxRelativePathBytes = 384;
static constexpr size_t kWebNasMaxListNameBytes = 192;
static constexpr size_t kWebNasBaseConfigMaxBytes = 2048;
static constexpr size_t kWebNasSourcesConfigMaxBytes = 12 * 1024;
static constexpr uint8_t kWebNasCommitBaseBit = 0x01;
static constexpr uint8_t kWebNasCommitSourcesBit = 0x02;
static constexpr uint8_t kWebNasCommitAllBits =
    kWebNasCommitBaseBit | kWebNasCommitSourcesBit;

struct WebRadioLogoUploadState {
  uint8_t* data = nullptr;
  size_t size = 0;
  size_t capacity = 0;
  int slot = -1;
  bool failed = false;
  bool result_ready = false;
  bool staged = false;
  bool deferred = false;
  char path[192] = {};
  char error[128] = {};
};

enum class WebSystemAssetKind : uint8_t {
  None = 0,
  DefaultCover,
  NetCoverLoading,
};

struct WebSystemAssetUploadState {
  uint8_t* data = nullptr;
  size_t size = 0;
  size_t capacity = 0;
  WebSystemAssetKind kind = WebSystemAssetKind::None;
  bool failed = false;
  bool result_ready = false;
  bool staged = false;
  bool deferred = false;
  char error[128] = {};
};

static bool s_wifi_enabled = true;

static WebServer s_server(80);
static bool s_started = false;
static bool s_ready = false;
static TaskHandle_t s_web_start_task = nullptr;
static bool s_web_start_in_progress = false;
static portMUX_TYPE s_web_start_task_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_ap_mode = false;
static bool s_wifi_config_apply_pending = false;
static uint32_t s_wifi_config_apply_at_ms = 0;
static portMUX_TYPE s_wifi_config_apply_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_nas_config_apply_requested = false;
static bool s_nas_config_apply_pending = false;
static uint32_t s_nas_config_apply_at_ms = 0;
static uint8_t s_nas_config_commit_success_mask = 0;
static uint8_t s_nas_config_requested_source_index = 0;
static uint16_t s_nas_config_commit_generation = 0;
static portMUX_TYPE s_nas_config_apply_mux = portMUX_INITIALIZER_UNLOCKED;
static WebRadioLogoUploadState s_radio_logo_upload{};
static WebSystemAssetUploadState s_system_asset_upload{};

static void web_schedule_wifi_config_apply(uint32_t delay_ms)
{
  portENTER_CRITICAL(&s_wifi_config_apply_mux);
  s_wifi_config_apply_pending = true;
  s_wifi_config_apply_at_ms = millis() + delay_ms;
  portEXIT_CRITICAL(&s_wifi_config_apply_mux);
}

static bool web_wifi_config_apply_is_pending()
{
  portENTER_CRITICAL(&s_wifi_config_apply_mux);
  const bool pending = s_wifi_config_apply_pending;
  portEXIT_CRITICAL(&s_wifi_config_apply_mux);
  return pending;
}

// Web 音量锁由状态页和控制接口共同访问，统一通过短临界区读取和发布。
struct WebUiControlSnapshot {
  bool volume_locked = true;
  uint32_t revision = 1;
};

static WebUiControlSnapshot s_web_ui_control{};
static portMUX_TYPE s_web_ui_control_mux = portMUX_INITIALIZER_UNLOCKED;

static WebUiControlSnapshot web_ui_control_snapshot_get()
{
  portENTER_CRITICAL(&s_web_ui_control_mux);
  const WebUiControlSnapshot snapshot = s_web_ui_control;
  portEXIT_CRITICAL(&s_web_ui_control_mux);
  return snapshot;
}

static void web_ui_volume_locked_set(bool locked)
{
  portENTER_CRITICAL(&s_web_ui_control_mux);
  if (s_web_ui_control.volume_locked != locked) {
    s_web_ui_control.volume_locked = locked;
    ++s_web_ui_control.revision;
    if (s_web_ui_control.revision == 0) {
      ++s_web_ui_control.revision;
    }
  }
  portEXIT_CRITICAL(&s_web_ui_control_mux);
}

static uint32_t web_status_token_add_u32(uint32_t hash, uint32_t value)
{
  for (uint8_t i = 0; i < 4; ++i) {
    hash ^= static_cast<uint8_t>((value >> (i * 8)) & 0xFF);
    hash *= 16777619u;
  }
  return hash;
}

static uint32_t web_status_token_add_bytes(uint32_t hash, const char* text)
{
  if (!text) return hash;
  const uint8_t* p = reinterpret_cast<const uint8_t*>(text);
  while (*p) {
    hash ^= *p++;
    hash *= 16777619u;
  }
  return hash;
}

/**
 * @brief 生成不包含播放时间的网页状态版本号。
 *
 * 播放时间由浏览器本地单调时钟连续推进；只有歌曲、暂停、跳转、音量、
 * 歌词、封面等真正状态变化时，才需要重新拉取完整 /api/status。
 */
static uint32_t web_status_state_token(const WebUiControlSnapshot& ui_control,
                                       AudioSeekStateSnapshot* out_seek = nullptr,
                                       AppRescanState* out_rescan = nullptr)
{
  const AppRescanState rescan = app_rescan_state_get();
  const AppPlayModeSnapshot play_mode = app_play_mode_snapshot_get();
  const PlayerSourceRuntimeState source = player_source_runtime_get();
  const WebRuntimeSettings settings = web_settings_get();

  AudioSeekStateSnapshot seek{};
  (void)audio_service_get_seek_state(&seek);

  AudioNetworkStateSnapshot network{};
  (void)audio_service_get_network_state(&network);

  uint32_t hash = 2166136261u;
  hash = web_status_token_add_u32(hash, audio_service_playback_revision());
  hash = web_status_token_add_u32(hash, seek.revision);
  hash = web_status_token_add_u32(hash, static_cast<uint32_t>(g_app_state));
  hash = web_status_token_add_u32(hash, rescan.rescanning ? 1u : 0u);
  hash = web_status_token_add_u32(hash, rescan.abort_requested ? 1u : 0u);
  hash = web_status_token_add_u32(hash, audio_service_is_playing() ? 1u : 0u);
  hash = web_status_token_add_u32(hash, audio_service_is_paused() ? 1u : 0u);
  hash = web_status_token_add_u32(hash, audio_service_is_seekable() ? 1u : 0u);
  hash = web_status_token_add_u32(hash, audio_get_total_ms());
  hash = web_status_token_add_u32(hash, audio_output_route_get_user_volume());
  hash = web_status_token_add_u32(hash, play_mode.revision);
  hash = web_status_token_add_u32(hash, static_cast<uint32_t>(play_mode.mode));
  hash = web_status_token_add_u32(hash, static_cast<uint32_t>(ui_get_view()));
  hash = web_status_token_add_u32(hash, static_cast<uint32_t>(source.type));
  hash = web_status_token_add_u32(hash, source.radio_active ? 1u : 0u);
  hash = web_status_token_add_u32(hash, source.net_track_active ? 1u : 0u);
  hash = web_status_token_add_u32(hash, static_cast<uint32_t>(player_state_current_index()));
  hash = web_status_token_add_u32(hash, ui_control.revision);

  hash = web_status_token_add_u32(hash, static_cast<uint32_t>(settings.refresh_preset));
  hash = web_status_token_add_u32(hash, static_cast<uint32_t>(settings.lyric_sync_mode));
  hash = web_status_token_add_u32(hash, settings.show_next_lyric ? 1u : 0u);
  hash = web_status_token_add_u32(hash, settings.show_cover ? 1u : 0u);
  hash = web_status_token_add_u32(hash, settings.web_cover_spin ? 1u : 0u);

  hash = web_status_token_add_u32(hash, g_lyricsDisplay.hasLyrics() ? 1u : 0u);
  hash = web_status_token_add_u32(hash, g_lyricsDisplay.getCurrentLyricStartTime());
  hash = web_status_token_add_u32(hash, g_lyricsDisplay.getNextLyricStartTime());
  hash = web_status_token_add_u32(hash, g_lyricsDisplay.getFollowingLyricStartTime());
  hash = web_status_token_add_u32(hash, web_cover_cache_revision());

  hash = web_status_token_add_u32(hash, static_cast<uint32_t>(network.start_phase));
  hash = web_status_token_add_u32(hash, network.active ? 1u : 0u);
  hash = web_status_token_add_u32(hash, network.waiting_for_data ? 1u : 0u);
  hash = web_status_token_add_u32(hash, network.reconnecting ? 1u : 0u);
  hash = web_status_token_add_u32(hash, network.eof ? 1u : 0u);
  hash = web_status_token_add_bytes(hash, network.error);

  if (out_seek) *out_seek = seek;
  if (out_rescan) *out_rescan = rescan;
  return hash == 0 ? 1 : hash;
}

static bool web_start_task_try_reserve()
{
    portENTER_CRITICAL(&s_web_start_task_mux);
    if (s_web_start_in_progress) {
        portEXIT_CRITICAL(&s_web_start_task_mux);
        return false;
    }

    s_web_start_in_progress = true;
    s_web_start_task = nullptr;
    portEXIT_CRITICAL(&s_web_start_task_mux);
    return true;
}

static void web_start_task_publish_handle(TaskHandle_t task)
{
    portENTER_CRITICAL(&s_web_start_task_mux);
    if (s_web_start_in_progress) {
        s_web_start_task = task;
    }
    portEXIT_CRITICAL(&s_web_start_task_mux);
}

static void web_start_task_finish(TaskHandle_t task)
{
    portENTER_CRITICAL(&s_web_start_task_mux);
    if (!task || !s_web_start_task || s_web_start_task == task) {
        s_web_start_task = nullptr;
        s_web_start_in_progress = false;
    }
    portEXIT_CRITICAL(&s_web_start_task_mux);
}

bool web_wifi_is_enabled()
{
    return s_wifi_enabled;
}

static bool web_network_audio_source_active()
{
    const PlayerSourceState source = player_source_get();
    return source.type == PlayerSourceType::NET_RADIO ||
           source.type == PlayerSourceType::NET_TRACK;
}

static void web_stop_network_audio_before_wifi_down(const char* reason)
{
    if (!web_network_audio_source_active()) {
        return;
    }

    // 网络音频可能仍处于 DNS、TCP 连接、响应头读取或预缓冲阶段，
    // 此时 is_playing()/is_paused() 都可能为 false，但 WiFiClient 仍在 AudioTask 中使用。
    // 因此只要当前来源是网络电台或 NAS，就必须先取消网络操作并等待停止命令完成。
    LOGW("[网页] WiFi 关闭前先停止网络音频：%s", reason ? reason : "未知");
    if (!audio_service_stop(true)) {
        // audio_service_stop() 在入队前已经递增取消编号；即使等待超时，
        // 正在进行的连接/读取流程也会在下一次取消检查时退出。
        LOGW("[网页] 等待网络音频停止超时，继续执行 WiFi 切换");
    }
}

void web_wifi_set_enabled(bool enabled)
{
#if WEBCTRL_ENABLED
    if (s_wifi_enabled == enabled) {
        return;
    }

    s_wifi_enabled = enabled;

    // WiFi 总开关保存到 NVS。
    // 关闭后，下次开机不再自动连接 WiFi。
    WebRuntimeSettings ws = web_settings_get();
    ws.wifi_enabled = enabled;
    web_settings_set(ws);
    (void)web_settings_save();

    if (!enabled) {
        LOGW("[网页] 用户已关闭 WiFi");

        // 停止 Web 服务
        if (s_started) {
            s_server.stop();
        }

        s_started = false;
        s_ready = false;
        s_ap_mode = false;

        // 关闭 WiFi 前先停掉网络音频。
        // 否则 AudioTask 可能正在 WiFiClient::read()，此时直接断 WiFi 会触发 lwIP pbuf 断言。
        web_stop_network_audio_before_wifi_down("user disabled WiFi");

        WiFi.softAPdisconnect(true);
        WiFi.disconnect(true, true);
        WiFi.mode(WIFI_OFF);

        quick_menu_request_refresh();
        return;
    }

    LOGI("[网页] 用户已启用 WiFi");

    WiFi.mode(WIFI_STA);

    // 重新启动 Web/WiFi 流程，不阻塞菜单/UI。
    web_server_start_async();
    quick_menu_request_refresh();
#endif
}

void web_wifi_toggle()
{
    web_wifi_set_enabled(!s_wifi_enabled);
}

static String web_trim_copy(const String& in) { String s = in; s.trim(); return s; }
static String web_json_escape(const String& in) {
  String out; out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); ++i) {
    char c = in[i];
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"':   out += "\\\""; break;
      case '\n':  out += "\\n"; break;
      case '\r':  out += "\\r"; break;
      case '\t':  out += "\\t"; break;
      default:    out += c; break;
    }
  }
  return out;
}
static uint32_t web_fnv1a32_add_bytes(uint32_t h, const char* s) {
  if (!s) return h;
  const uint8_t* p = reinterpret_cast<const uint8_t*>(s);
  while (*p) {
    h ^= *p++;
    h *= 16777619u;
  }
  return h;
}

static uint32_t web_fnv1a32_add_u32(uint32_t h, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    h ^= (uint8_t)((v >> (i * 8)) & 0xFF);
    h *= 16777619u;
  }
  return h;
}

static String web_make_track_cover_rev(const TrackViewV3& v) {
  uint32_t h = 2166136261u;
  h = web_fnv1a32_add_u32(h, (uint32_t)v.cover_source);
  h = web_fnv1a32_add_u32(h, v.cover_offset);
  h = web_fnv1a32_add_u32(h, v.cover_size);
  h = web_fnv1a32_add_bytes(h, v.audio_path.c_str());
  h = web_fnv1a32_add_bytes(h, v.cover_path.c_str());

  char buf[16];
  snprintf(buf, sizeof(buf), "%08lx", (unsigned long)h);
  return String(buf);
}



static bool web_if_none_match_hit(const String& etag) {
  if (!s_server.hasHeader("If-None-Match")) return false;
  const String inm = s_server.header("If-None-Match");
  if (inm.length() == 0) return false;
  if (inm == "*") return true;
  return inm.indexOf(etag) >= 0;
}

static void web_send_not_modified(const String& etag) {
  WiFiClient client = s_server.client();
  client.print("HTTP/1.1 304 Not Modified\r\n");
  client.printf("ETag: %s\r\n", etag.c_str());
  client.print("Cache-Control: public, max-age=86400, immutable\r\n");
  client.print("Connection: close\r\n");
  client.print("\r\n");
  client.flush();
}
static void web_json_append_escaped(String& out, const char* s) {
  if (!s) return;
  for (const char* p = s; *p; ++p) {
    const char c = *p;
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"':  out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:   out += c; break;
    }
  }
}

static const char* web_track_album_name_cstr(const MusicCatalogV3& cat, const TrackRowV3& row) {
  if (row.album_id == INVALID_ID32 || !cat.albums || row.album_id >= cat.album_count) {
    return "";
  }
  return pool_str_v3(cat.pool, cat.albums[row.album_id].name_off);
}

static bool web_str_icontains(const char* s, const String& q_lower) {
  if (!s || !s[0] || q_lower.length() == 0) return false;
  String t = String(s);
  t.toLowerCase();
  return t.indexOf(q_lower) >= 0;
}

static void web_json_append_match_titles(String& json, const String& titles_text) {
  json += ",\"matched_titles_text\":\"";
  json += web_json_escape(titles_text);
  json += "\"";
}

static void web_abort_client() {
  WiFiClient client = s_server.client();
  client.stop();
}

static bool web_client_alive() {
  WiFiClient client = s_server.client();
  return client.connected();
}

static bool web_send_chunk(const char* s) {
  if (!web_client_alive()) { web_abort_client(); return false; }
  s_server.sendContent(s);
  if (!web_client_alive()) { web_abort_client(); return false; }
  return true;
}

static bool web_send_chunk(const String& s) {
  if (!web_client_alive()) { web_abort_client(); return false; }
  s_server.sendContent(s);
  if (!web_client_alive()) { web_abort_client(); return false; }
  return true;
}

static void web_end_stream_response() {
  s_server.sendContent("");
}

static bool web_flush_chunk_buffer(String& buf) {
  if (!buf.length()) return true;
  bool ok = web_send_chunk(buf);
  buf = "";
  return ok;
}


static void web_send_no_cache_headers();

// /api/status 是网页最高频的接口。旧实现每次创建约 3KB String，
// 并通过大量 String 临时对象拼接，长时间轮询会增加内部堆碎片。
// 这里使用与刷新阈值一致的常驻缓冲区分块输出，避免额外保留未使用容量。
static constexpr size_t kWebStatusJsonChunkReserve = 768;
static constexpr size_t kWebStatusJsonFlushAt = 768;
static String s_web_status_json_chunk;
static WebPlayerSnapshot s_web_status_snapshot;
static uint32_t s_web_status_stream_requests = 0;
static size_t s_web_status_stream_max_bytes = 0;
static uint16_t s_web_status_stream_max_chunks = 0;

class WebStatusJsonWriter {
 public:
  explicit WebStatusJsonWriter(String& buffer) : buffer_(buffer) {}

  bool prepare() {
    buffer_.remove(0);
    ok_ = buffer_.reserve(kWebStatusJsonChunkReserve);
    first_field_ = true;
    started_ = false;
    total_bytes_ = 1;  // 起始的“{”由 WebServer 首包发送。
    chunk_count_ = 1;
    return ok_;
  }

  bool begin() {
    if (!ok_) return false;
    web_send_no_cache_headers();
    s_server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    s_server.send(200, "application/json; charset=utf-8", "{");
    started_ = web_client_alive();
    ok_ = started_;
    return ok_;
  }

  void field_bool(const char* key, bool value) {
    if (!field_prefix(key)) return;
    append_literal(value ? "true" : "false");
  }

  void field_int(const char* key, int32_t value) {
    if (!field_prefix(key)) return;
    char buf[24];
    snprintf(buf, sizeof(buf), "%ld", static_cast<long>(value));
    append_literal(buf);
  }

  void field_uint(const char* key, uint32_t value) {
    if (!field_prefix(key)) return;
    char buf[24];
    snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(value));
    append_literal(buf);
  }

  void field_string(const char* key, const String& value) {
    field_string(key, value.c_str());
  }

  void field_string(const char* key, const char* value) {
    if (!field_prefix(key)) return;
    append_char('"');
    append_escaped(value ? value : "");
    append_char('"');
  }

  bool finish() {
    if (!ok_ || !started_) return false;
    append_char('}');
    if (!flush()) return false;
    web_end_stream_response();

    ++s_web_status_stream_requests;
    if (total_bytes_ > s_web_status_stream_max_bytes) {
      s_web_status_stream_max_bytes = total_bytes_;
    }
    if (chunk_count_ > s_web_status_stream_max_chunks) {
      s_web_status_stream_max_chunks = chunk_count_;
    }

    if ((s_web_status_stream_requests % 120u) == 0u) {
      LOGD("[网页] /api/status 流式统计：请求=%lu 最大响应=%uB 最大分块=%u 缓冲=%uB",
           static_cast<unsigned long>(s_web_status_stream_requests),
           static_cast<unsigned>(s_web_status_stream_max_bytes),
           static_cast<unsigned>(s_web_status_stream_max_chunks),
           static_cast<unsigned>(kWebStatusJsonChunkReserve));
    }
    return true;
  }

 private:
  bool field_prefix(const char* key) {
    if (!ok_) return false;
    if (!first_field_) append_char(',');
    first_field_ = false;
    append_char('"');
    append_literal(key ? key : "");
    append_literal("\":");
    return ok_;
  }

  void append_escaped(const char* value) {
    if (!value) return;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(value);
    while (*p && ok_) {
      const uint8_t c = *p++;
      switch (c) {
        case '\\': append_literal("\\\\"); break;
        case '"':  append_literal("\\\""); break;
        case '\b': append_literal("\\b"); break;
        case '\f': append_literal("\\f"); break;
        case '\n': append_literal("\\n"); break;
        case '\r': append_literal("\\r"); break;
        case '\t': append_literal("\\t"); break;
        default:
          if (c < 0x20) {
            char escaped[7];
            snprintf(escaped, sizeof(escaped), "\\u%04X", static_cast<unsigned>(c));
            append_literal(escaped);
          } else {
            append_char(static_cast<char>(c));
          }
          break;
      }
    }
  }

  void append_literal(const char* text) {
    if (!text || !ok_) return;
    while (*text && ok_) {
      if (buffer_.length() >= kWebStatusJsonFlushAt && !flush()) return;
      const size_t room = kWebStatusJsonFlushAt - buffer_.length();
      const size_t remaining = strlen(text);
      const size_t take = remaining < room ? remaining : room;
      if (take == 0) {
        if (!flush()) return;
        continue;
      }
      if (!buffer_.concat(text, static_cast<unsigned int>(take))) {
        ok_ = false;
        return;
      }
      text += take;
    }
  }

  void append_char(char c) {
    if (!ok_) return;
    if (buffer_.length() >= kWebStatusJsonFlushAt && !flush()) return;
    if (!buffer_.concat(c)) {
      ok_ = false;
    }
  }

  bool flush() {
    if (!ok_) return false;
    if (!buffer_.length()) return true;
    const size_t bytes = buffer_.length();
    if (!web_send_chunk(buffer_)) {
      ok_ = false;
      return false;
    }
    total_bytes_ += bytes;
    ++chunk_count_;
    // remove(0) 清空内容但保留已申请容量，下一次轮询不会重新分配。
    buffer_.remove(0);
    return true;
  }

  String& buffer_;
  bool ok_ = false;
  bool first_field_ = true;
  bool started_ = false;
  size_t total_bytes_ = 0;
  uint16_t chunk_count_ = 0;
};

static bool web_parse_bool(const String& v, bool defv=false) {
  String s = web_trim_copy(v); s.toLowerCase();
  if (s=="1"||s=="true"||s=="yes"||s=="on") return true;
  if (s=="0"||s=="false"||s=="no"||s=="off") return false;
  return defv;
}

static bool web_settings_persistent_core_changed(const WebRuntimeSettings& old_cfg,
                                                 const WebRuntimeSettings& new_cfg)
{
  // 硬件控制设置立即写入 NVS；网页显示设置可延迟到关机前保存。
  return old_cfg.refresh_preset != new_cfg.refresh_preset
      || old_cfg.lyric_sync_mode != new_cfg.lyric_sync_mode
      || old_cfg.wifi_enabled != new_cfg.wifi_enabled
      || old_cfg.hall_control_enabled != new_cfg.hall_control_enabled
      || old_cfg.solenoid_enabled != new_cfg.solenoid_enabled
      || old_cfg.solenoid_direction_inverted != new_cfg.solenoid_direction_inverted
      || old_cfg.status_led_enabled != new_cfg.status_led_enabled
      || old_cfg.status_led_brightness != new_cfg.status_led_brightness;
}

static const char* web_device_view_key(ui_player_view_t view)
{
  switch (view) {
    case UI_VIEW_ROTATE:      return "rotate";
    case UI_VIEW_COVER_PANEL: return "cover_panel";
    case UI_VIEW_INFO:
    default:                  return "info";
  }
}

static bool web_parse_device_view(const String& value, ui_player_view_t* out_view)
{
  if (!out_view) return false;
  String key = web_trim_copy(value);
  key.toLowerCase();
  if (key == "info") {
    *out_view = UI_VIEW_INFO;
    return true;
  }
  if (key == "rotate") {
    *out_view = UI_VIEW_ROTATE;
    return true;
  }
  if (key == "cover_panel" || key == "panel") {
    *out_view = UI_VIEW_COVER_PANEL;
    return true;
  }
  return false;
}

static bool web_sleep_timer_minutes_valid(uint16_t minutes)
{
  return minutes == 0 || minutes == 15 || minutes == 30 || minutes == 60 || minutes == 90;
}
static bool web_parse_mac(const String& text, uint8_t out[6]) {
  unsigned vals[6];
  if (sscanf(text.c_str(), "%x:%x:%x:%x:%x:%x", &vals[0], &vals[1], &vals[2], &vals[3], &vals[4], &vals[5]) != 6) return false;
  for (int i = 0; i < 6; ++i) out[i] = (uint8_t)vals[i];
  return true;
}
static String web_ip_string() {
  if (s_ap_mode) return WiFi.softAPIP().toString();
  if (WiFi.status() == WL_CONNECTED) return WiFi.localIP().toString();
  return String("0.0.0.0");
}
static const char* web_net_mode_cstr() {
  if (s_ap_mode) return "AP";
  if (WiFi.status() == WL_CONNECTED) return "STA";
  return "OFFLINE";
}
static void web_send_no_cache_headers() {
  s_server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  s_server.sendHeader("Pragma", "no-cache");
  s_server.sendHeader("Expires", "0");
}

static void web_send_json_ok_simple(const char* msg = nullptr) {
  web_send_no_cache_headers();

  String json = "{\"ok\":true";
  if (msg && *msg) {
    json += ",\"message\":\"";
    json += web_json_escape(msg);
    json += "\"";
  }
  json += "}";

  s_server.send(200, "application/json; charset=utf-8", json);  
}

static const char* web_nfc_type_label_cn(NfcBindType type) {
  switch (type) {
    case NFC_BIND_TRACK:  return "单曲";
    case NFC_BIND_ARTIST: return "歌手";
    case NFC_BIND_ALBUM:  return "专辑";
    default:              return "未知";
  }
}

static bool web_nfc_test_play_by_uid(const String& uid) {
  NfcBindingEntry entry;
  if (!nfc_binding_find(uid, entry)) return false;

  switch (entry.type) {
    case NFC_BIND_TRACK: {
      const int idx = player_recover_find_track_idx_by_path(entry.key);
      if (idx < 0) return false;
      return player_binding_try_handle_nfc_uid(uid);
    }

    case NFC_BIND_ARTIST:
      return player_play_artist_binding(entry.key);

    case NFC_BIND_ALBUM:
      return player_play_album_binding(entry.key);

    default:
      return false;
  }
}
static void web_send_json_err(const char* msg, int code = 400) {
  web_send_no_cache_headers();

  String json = "{\"ok\":false,\"message\":\"";
  json += web_json_escape(msg ? String(msg) : String("error"));
  json += "\"}";

  s_server.send(code, "application/json; charset=utf-8", json);  
}
static bool web_require_player_state() {
  if (g_app_state != STATE_PLAYER) { web_send_json_err("当前不在播放器主界面"); return false; }
  if (app_rescan_state_get().rescanning) { web_send_json_err("正在扫描音乐库"); return false; }
  if (player_list_select_is_active()) { web_send_json_err("当前处于列表选择模式"); return false; }
  return true;
}

static bool web_wifi_set_error(String* error, const char* message)
{
  if (error) *error = message ? message : "wifi_config_invalid";
  return false;
}

static bool web_wifi_text_value_valid(const String& value,
                                      size_t max_bytes,
                                      bool allow_empty)
{
  if ((!allow_empty && value.isEmpty()) || value.length() > max_bytes) {
    return false;
  }
  for (size_t i = 0; i < value.length(); ++i) {
    const uint8_t ch = static_cast<uint8_t>(value[i]);
    if (ch == 0 || ch == '\r' || ch == '\n' || ch < 0x20) {
      return false;
    }
  }
  return true;
}

static bool web_wifi_hostname_valid(const String& hostname)
{
  if (!web_wifi_text_value_valid(hostname,
                                 kWebWifiMaxHostnameBytes,
                                 false)) {
    return false;
  }
  if (hostname[0] == '-' || hostname[hostname.length() - 1] == '-') {
    return false;
  }
  for (size_t i = 0; i < hostname.length(); ++i) {
    const char ch = hostname[i];
    const bool alpha_num = (ch >= 'a' && ch <= 'z') ||
                           (ch >= 'A' && ch <= 'Z') ||
                           (ch >= '0' && ch <= '9');
    if (!alpha_num && ch != '-') return false;
  }
  return true;
}

static bool web_wifi_network_valid(const WebWifiNetwork& network,
                                   String* error)
{
  if (!web_wifi_text_value_valid(network.ssid,
                                 kWebWifiMaxSsidBytes,
                                 false)) {
    return web_wifi_set_error(error, "SSID不能为空，且最多32字节");
  }
  if (!web_wifi_text_value_valid(network.password,
                                 kWebWifiMaxPasswordBytes,
                                 true)) {
    return web_wifi_set_error(error, "WiFi密码最多64字节，不能包含换行或控制字符");
  }
  if (network.channel < 0 || network.channel > 14) {
    return web_wifi_set_error(error, "WiFi信道必须为0或1到14");
  }
  if (network.has_bssid && network.bssid_text.isEmpty()) {
    return web_wifi_set_error(error, "BSSID格式无效");
  }
  return true;
}

static bool web_wifi_push_network(std::vector<WebWifiNetwork>& nets,
                                  const WebWifiNetwork& network,
                                  String* error)
{
  if (network.ssid.isEmpty()) return true;
  if (nets.size() >= kWebWifiMaxNetworks) {
    return web_wifi_set_error(error, "WiFi配置数量超过上限");
  }
  nets.push_back(network);
  return true;
}

using WebWifiReadLineCallback = bool (*)(void* context, String& line);

static bool web_parse_wifi_config_lines(WebWifiReadLineCallback read_line,
                                        void* read_context,
                                        size_t source_size,
                                        std::vector<WebWifiNetwork>& nets,
                                        String& hostname,
                                        String* error)
{
  nets.clear();
  hostname = WEBCTRL_HOSTNAME_DEFAULT;
  if (error) error->remove(0);

  if (!read_line || source_size > 16 * 1024) {
    return web_wifi_set_error(error, "WiFi配置文件过大");
  }

  WebWifiNetwork cur{};
  bool in_network = false;
  String line;
  while (read_line(read_context, line)) {
    if (line.length() > 256) {
      return web_wifi_set_error(error, "WiFi配置存在过长行");
    }
    line.trim();
    if (line.isEmpty() || line.startsWith("#") || line.startsWith(";")) {
      continue;
    }

    if (line.startsWith("[")) {
      if (in_network && !web_wifi_push_network(nets, cur, error)) {
        return false;
      }
      cur = WebWifiNetwork{};
      in_network = line.equalsIgnoreCase("[network]");
      continue;
    }

    const int eq = line.indexOf('=');
    if (eq <= 0) continue;

    String key = web_trim_copy(line.substring(0, eq));
    String val = web_trim_copy(line.substring(eq + 1));
    key.toLowerCase();

    if (!in_network) {
      if (key == "hostname" && val.length()) hostname = val;
      continue;
    }

    if (key == "ssid") {
      cur.ssid = val;
    } else if (key == "password") {
      cur.password = val;
    } else if (key == "hidden") {
      cur.hidden = web_parse_bool(val, false);
    } else if (key == "channel") {
      long channel = val.toInt();
      if (channel < 0) channel = 0;
      cur.channel = static_cast<int>(channel);
    } else if (key == "bssid") {
      cur.has_bssid = web_parse_mac(val, cur.bssid);
      cur.bssid_text = cur.has_bssid ? val : String("");
    }
  }

  if (in_network && !web_wifi_push_network(nets, cur, error)) {
    return false;
  }
  if (nets.empty()) {
    return web_wifi_set_error(error, "WiFi配置中没有可用网络");
  }
  return true;
}

static bool web_wifi_read_file_line(void* context, String& line)
{
  File32* file = static_cast<File32*>(context);
  if (!file || !file->available()) return false;
  line = file->readStringUntil('\n');
  return true;
}

struct WebWifiMemoryReader {
  const uint8_t* data = nullptr;
  size_t size = 0;
  size_t offset = 0;
};

static bool web_wifi_read_memory_line(void* context, String& line)
{
  WebWifiMemoryReader* reader = static_cast<WebWifiMemoryReader*>(context);
  if (!reader || !reader->data || reader->offset >= reader->size) return false;

  line.remove(0);
  while (reader->offset < reader->size) {
    const char ch = static_cast<char>(reader->data[reader->offset++]);
    if (ch == '\n') break;
    line += ch;
    if (line.length() > 256) break;
  }
  return true;
}

static bool web_parse_wifi_config_file(File32& file,
                                       std::vector<WebWifiNetwork>& nets,
                                       String& hostname,
                                       String* error)
{
  return web_parse_wifi_config_lines(web_wifi_read_file_line,
                                     &file,
                                     file.fileSize(),
                                     nets,
                                     hostname,
                                     error);
}

static bool web_parse_wifi_config_memory(const uint8_t* data,
                                         size_t size,
                                         std::vector<WebWifiNetwork>& nets,
                                         String& hostname,
                                         String* error)
{
  WebWifiMemoryReader reader{};
  reader.data = data;
  reader.size = size;
  return web_parse_wifi_config_lines(web_wifi_read_memory_line,
                                     &reader,
                                     size,
                                     nets,
                                     hostname,
                                     error);
}

static bool web_validate_wifi_config_file(File32& file, void*)
{
  std::vector<WebWifiNetwork> nets;
  String hostname;
  String error;
  return web_parse_wifi_config_file(file, nets, hostname, &error);
}

static bool web_load_wifi_config(std::vector<WebWifiNetwork>& nets,
                                 String& hostname,
                                 String* error = nullptr,
                                 bool* loaded_from_pending = nullptr)
{
  if (loaded_from_pending) *loaded_from_pending = false;

  const size_t pending_size =
      storage_config_pending_size(SystemPaths::kWifiConfig);
  if (pending_size > 0 && pending_size <= 16 * 1024) {
    uint8_t* pending = static_cast<uint8_t*>(heap_caps_malloc(
        pending_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (pending) {
      size_t copied = 0;
      const bool copied_ok = storage_config_read_pending(
          SystemPaths::kWifiConfig, pending, pending_size, &copied);
      const bool parsed_ok = copied_ok &&
          web_parse_wifi_config_memory(pending, copied, nets, hostname, error);
      heap_caps_free(pending);
      if (parsed_ok) {
        if (loaded_from_pending) *loaded_from_pending = true;
        LOGD("[网页] WiFi配置从PSRAM待写区读取：网络数量=%d",
             (int)nets.size());
        return true;
      }
    }
  }

  (void)storage_config_recover(SystemPaths::kWifiConfig,
                               web_validate_wifi_config_file,
                               nullptr,
                               1200);

  StorageSdLockGuard guard(1200);
  if (!guard) {
    LOGW("[网页] 跳过 WiFi 配置读取：获取 SD 锁失败");
    return web_wifi_set_error(error, "等待TF卡访问超时");
  }

  File32 file = sd.open(SystemPaths::kWifiConfig, O_RDONLY);
  if (!file) {
    LOGW("[网页] 未找到 WiFi 配置：%s", SystemPaths::kWifiConfig);
    return web_wifi_set_error(error, "WiFi配置文件不存在");
  }

  const bool ok = web_parse_wifi_config_file(file,
                                              nets,
                                              hostname,
                                              error);
  file.close();
  if (!ok) {
    LOGW("[网页] WiFi 配置读取失败：%s",
         error && error->length() ? error->c_str() : "格式无效");
    return false;
  }

  LOGD("[网页] WiFi 配置已读取：网络数量=%d 主机名=%s",
       (int)nets.size(),
       hostname.c_str());
  return true;
}

static String web_wifi_format_bssid(const WebWifiNetwork& network)
{
  if (!network.has_bssid) return String("");
  char text[18] = {};
  snprintf(text,
           sizeof(text),
           "%02X:%02X:%02X:%02X:%02X:%02X",
           network.bssid[0],
           network.bssid[1],
           network.bssid[2],
           network.bssid[3],
           network.bssid[4],
           network.bssid[5]);
  return String(text);
}

static bool web_serialize_wifi_config(
    const std::vector<WebWifiNetwork>& networks,
    const String& hostname,
    String& output)
{
  output.remove(0);
  output.reserve(192 + networks.size() * 180);
  output += "# ESP32S3 Player WiFi configuration\r\n";
  output += "# 由设备网页配置中心保存；本地播放时先暂存PSRAM，切歌安全窗口再写入TF卡\r\n";
  output += "hostname=";
  output += hostname;
  output += "\r\n";

  for (const WebWifiNetwork& network : networks) {
    output += "\r\n[network]\r\nssid=";
    output += network.ssid;
    output += "\r\npassword=";
    output += network.password;
    output += "\r\nhidden=";
    output += network.hidden ? "1" : "0";
    output += "\r\nchannel=";
    output += String(network.channel);
    output += "\r\nbssid=";
    output += web_wifi_format_bssid(network);
    output += "\r\n";
  }

  return output.length() > 0 && output.length() <= 16 * 1024;
}

static void web_wifi_config_commit_complete(const char* final_path,
                                            bool success,
                                            void* context)
{
  const bool apply_after_commit =
      reinterpret_cast<uintptr_t>(context) != 0;
  if (!success) {
    LOGW("[网页] WiFi待写配置本次落盘失败，将在下一个安全窗口重试：%s",
         final_path ? final_path : "-");
    return;
  }

  LOGI("[网页] WiFi待写配置已在安全窗口落盘：%s",
       final_path ? final_path : "-");
  if (apply_after_commit) {
    // 回调只安排后续动作；真正断网仍在Web循环中延迟执行。
    web_schedule_wifi_config_apply(900);
  }
}

static String web_wifi_arg_name(size_t index, const char* field)
{
  char name[40] = {};
  snprintf(name, sizeof(name), "%s_%u", field, (unsigned)index);
  return String(name);
}

static String web_wifi_arg(size_t index, const char* field)
{
  return s_server.arg(web_wifi_arg_name(index, field));
}

static bool web_parse_wifi_save_request(std::vector<WebWifiNetwork>& networks,
                                        String& hostname,
                                        String& error)
{
  networks.clear();
  error.remove(0);

  hostname = web_trim_copy(s_server.arg("hostname"));
  if (!web_wifi_hostname_valid(hostname)) {
    error = "主机名只能包含字母、数字和连字符，长度1到63字节";
    return false;
  }

  const int requested_count = s_server.arg("count").toInt();
  if (requested_count <= 0 ||
      requested_count > static_cast<int>(kWebWifiMaxNetworks)) {
    error = "至少保留一个WiFi网络，最多16个";
    return false;
  }

  std::vector<WebWifiNetwork> old_networks;
  String old_hostname;
  String old_error;
  (void)web_load_wifi_config(old_networks, old_hostname, &old_error);

  networks.reserve(static_cast<size_t>(requested_count));
  for (int i = 0; i < requested_count; ++i) {
    WebWifiNetwork network{};
    network.ssid = web_trim_copy(web_wifi_arg(i, "ssid"));
    network.hidden = web_parse_bool(web_wifi_arg(i, "hidden"), false);

    const String channel_text = web_trim_copy(web_wifi_arg(i, "channel"));
    network.channel = channel_text.length() ? channel_text.toInt() : 0;

    const String bssid_text = web_trim_copy(web_wifi_arg(i, "bssid"));
    if (bssid_text.length()) {
      network.has_bssid = web_parse_mac(bssid_text, network.bssid);
      if (!network.has_bssid) {
        error = String("第") + String(i + 1) + "个网络的BSSID格式无效";
        return false;
      }
      network.bssid_text = web_wifi_format_bssid(network);
    }

    const bool keep_password =
        web_parse_bool(web_wifi_arg(i, "keep_password"), false);
    const int source_index = web_wifi_arg(i, "source_index").toInt();
    if (keep_password &&
        source_index >= 0 &&
        source_index < static_cast<int>(old_networks.size())) {
      network.password = old_networks[source_index].password;
    } else {
      network.password = s_server.arg(web_wifi_arg_name(i, "password"));
    }

    String network_error;
    if (!web_wifi_network_valid(network, &network_error)) {
      error = String("第") + String(i + 1) + "个网络：" + network_error;
      return false;
    }

    networks.push_back(network);
  }

  return true;
}

static bool web_stage_wifi_config_from_request(bool apply_after_commit,
                                               String& error,
                                               bool& deferred)
{
  deferred = false;

  std::vector<WebWifiNetwork> networks;
  String hostname;
  if (!web_parse_wifi_save_request(networks, hostname, error)) {
    return false;
  }

  String serialized;
  if (!web_serialize_wifi_config(networks, hostname, serialized)) {
    error = "WiFi配置序列化失败或超过16KB";
    return false;
  }

  void* callback_context = reinterpret_cast<void*>(
      static_cast<uintptr_t>(apply_after_commit ? 1u : 0u));
  if (!storage_config_stage_psram(
          SystemPaths::kWifiConfig,
          reinterpret_cast<const uint8_t*>(serialized.c_str()),
          serialized.length(),
          web_validate_wifi_config_file,
          nullptr,
          web_wifi_config_commit_complete,
          callback_context)) {
    error = "WiFi配置暂存PSRAM失败，原配置未改变";
    return false;
  }

  const bool local_audio_open =
      player_source_type_get() == PlayerSourceType::LOCAL_TRACK &&
      (audio_service_is_playing() || audio_service_is_paused());

  if (local_audio_open) {
    deferred = true;
  } else {
    // 所有配置都先进入PSRAM；当前没有本地音频文件占用时可立即走同一提交路径。
    (void)storage_config_commit_pending(4000);
    deferred = storage_config_has_pending_path(SystemPaths::kWifiConfig);
  }

  LOGI("[网页] WiFi配置已%s：网络=%u 主机名=%s",
       deferred ? "暂存PSRAM等待安全窗口" : "写入TF卡",
       (unsigned)networks.size(),
       hostname.c_str());
  return true;
}

static bool web_try_connect_one(const WebWifiNetwork& n, const String& hostname) {
  if (n.ssid.isEmpty()) return false;
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setHostname(hostname.c_str());
  WiFi.disconnect(true, true);
  delay(100);
  if (n.channel > 0 || n.has_bssid) {
    WiFi.begin(n.ssid.c_str(), n.password.c_str(), n.channel > 0 ? n.channel : 0, n.has_bssid ? n.bssid : nullptr, true);
  } else {
    WiFi.begin(n.ssid.c_str(), n.password.c_str());
  }
  LOGD("[网页] 正在连接 STA：ssid=%s%s", n.ssid.c_str(), n.hidden ? " (隐藏)" : "");
  const uint32_t t0 = millis();
  while ((millis() - t0) < WEBCTRL_STA_CONNECT_TIMEOUT_MS) {
    if (WiFi.status() == WL_CONNECTED) {
      WiFi.setSleep(false);
      LOGI("[网页] STA 已连接，IP=%s", WiFi.localIP().toString().c_str());
      s_ap_mode = false;
      return true;
    }
    delay(200);
  }
  LOGW("[网页] STA 连接超时：ssid=%s", n.ssid.c_str());
  return false;
}

static bool web_try_connect_sta_from_config() {
  // 如果 STA 已经连上，直接复用当前连接，不要为了启动 Web 服务再次 disconnect/reconnect。
  // 网络电台 / NAS HTTP 播放时，AudioTask 可能正在 WiFiClient::read()；
  // 另一个任务强制 WiFi.disconnect() 会破坏底层 socket/pbuf 生命周期。
  if (WiFi.status() == WL_CONNECTED) {
    WiFi.setSleep(false);
    s_ap_mode = false;
    LOGD("[网页] 复用已有 STA 连接，IP=%s", WiFi.localIP().toString().c_str());
    quick_menu_request_refresh();
    return true;
  }

  std::vector<WebWifiNetwork> nets;
  String hostname;
  if (!web_load_wifi_config(nets, hostname) || nets.empty()) return false;
  for (const auto& n : nets) {
    if (web_try_connect_one(n, hostname)) return true;
  }
  web_stop_network_audio_before_wifi_down("STA retry failed");
  WiFi.disconnect(true, true);
  return false;
}

static bool web_start_ap_fallback() {
  WiFi.mode(WIFI_AP);
  WiFi.setHostname(WEBCTRL_HOSTNAME_DEFAULT);
  const bool ok = WiFi.softAP(WEBCTRL_AP_SSID, WEBCTRL_AP_PASS);
  if (!ok) { LOGE("[网页] AP 启动失败"); return false; }
  WiFi.setSleep(false);
  s_ap_mode = true;
  LOGI("[网页] AP 已就绪：SSID=%s IP=%s", WEBCTRL_AP_SSID, WiFi.softAPIP().toString().c_str());
  return true;
}

static void web_apply_saved_wifi_config_now()
{
  if (!s_wifi_enabled) {
    LOGW("[网页] WiFi配置已保存，但WiFi总开关关闭，跳过重连");
    return;
  }

  LOGI("[网页] 开始应用网页保存的WiFi配置");
  web_stop_network_audio_before_wifi_down("apply saved WiFi config");

  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, true);
  delay(120);

  if (web_try_connect_sta_from_config()) {
    s_ap_mode = false;
    LOGI("[网页] 新WiFi配置已生效，IP=%s",
         WiFi.localIP().toString().c_str());
  } else {
    LOGW("[网页] 新WiFi配置连接失败，恢复AP兜底模式");
    (void)web_start_ap_fallback();
  }

  quick_menu_request_refresh();
}

static void web_process_pending_wifi_config_apply()
{
  const uint32_t now = millis();
  bool due = false;

  portENTER_CRITICAL(&s_wifi_config_apply_mux);
  if (s_wifi_config_apply_pending &&
      static_cast<int32_t>(now - s_wifi_config_apply_at_ms) >= 0) {
    s_wifi_config_apply_pending = false;
    due = true;
  }
  portEXIT_CRITICAL(&s_wifi_config_apply_mux);

  if (due) web_apply_saved_wifi_config_now();
}

static bool web_radio_set_error(String* error, const String& message)
{
  if (error) *error = message;
  return false;
}

static bool web_radio_text_valid(const String& value,
                                 size_t max_bytes,
                                 bool allow_empty)
{
  if ((!allow_empty && value.isEmpty()) || value.length() > max_bytes) {
    return false;
  }
  for (size_t i = 0; i < value.length(); ++i) {
    const uint8_t ch = static_cast<uint8_t>(value[i]);
    if (ch == 0 || ch == '\r' || ch == '\n' || ch < 0x20 || ch == '|') {
      return false;
    }
  }
  return true;
}

static bool web_radio_has_http_scheme(const String& value)
{
  if (value.length() < 7) return false;
  String prefix = value.substring(0, value.length() >= 8 ? 8 : 7);
  prefix.toLowerCase();
  return prefix.startsWith("http://") || prefix.startsWith("https://");
}

static bool web_radio_url_valid(const String& url)
{
  return web_radio_text_valid(url, kWebRadioMaxUrlBytes, false) &&
         web_radio_has_http_scheme(url);
}

static bool web_radio_logo_valid(const String& logo)
{
  if (!web_radio_text_valid(logo, kWebRadioMaxLogoBytes, true)) return false;
  if (logo.isEmpty()) return true;
  if (web_radio_has_http_scheme(logo)) return true;

  // 兼容旧版配置中已存在的 /System/ 本地台标路径。
  // 网页上传仍只会写入 /System/assets/radio/，这里只允许引用，不提供任意路径写入。
  return logo.startsWith("/System/") && logo.indexOf("..") < 0;
}

static bool web_radio_item_valid(const WebRadioConfigItem& item,
                                 String* error)
{
  if (!web_radio_text_valid(item.name, kWebRadioMaxNameBytes, false)) {
    return web_radio_set_error(error, "电台名称不能为空，最多96字节且不能包含竖线");
  }
  if (!web_radio_url_valid(item.url)) {
    return web_radio_set_error(error, "电台地址必须以http://或https://开头，最多512字节");
  }
  if (!web_radio_text_valid(item.format, kWebRadioMaxFormatBytes, true)) {
    return web_radio_set_error(error, "格式最多24字节，不能包含竖线或换行");
  }
  if (!web_radio_text_valid(item.region, kWebRadioMaxRegionBytes, true)) {
    return web_radio_set_error(error, "地区最多64字节，不能包含竖线或换行");
  }
  if (!web_radio_logo_valid(item.logo)) {
    return web_radio_set_error(error, "台标必须为空、远程图片URL或安全的/System/本地路径");
  }
  return true;
}

enum class WebRadioLineParseResult : uint8_t {
  Skip,
  Item,
  Invalid,
};

static WebRadioLineParseResult web_parse_radio_config_line(
    const String& raw,
    WebRadioConfigItem& item,
    String* reason)
{
  String line = web_trim_copy(raw);
  if (line.length() >= 3 &&
      static_cast<uint8_t>(line[0]) == 0xEF &&
      static_cast<uint8_t>(line[1]) == 0xBB &&
      static_cast<uint8_t>(line[2]) == 0xBF) {
    line.remove(0, 3);
    line.trim();
  }

  if (line.isEmpty() || line.startsWith("#") || line.startsWith(";")) {
    return WebRadioLineParseResult::Skip;
  }
  if (line.length() > 1024) {
    if (reason) *reason = "配置行超过1024字节";
    return WebRadioLineParseResult::Invalid;
  }

  const int p1 = line.indexOf('|');
  const int p2 = p1 >= 0 ? line.indexOf('|', p1 + 1) : -1;
  const int p3 = p2 >= 0 ? line.indexOf('|', p2 + 1) : -1;
  const int p4 = p3 >= 0 ? line.indexOf('|', p3 + 1) : -1;
  if (p1 <= 0) {
    if (reason) *reason = "缺少名称与地址之间的竖线分隔符";
    return WebRadioLineParseResult::Invalid;
  }

  item = WebRadioConfigItem{};
  item.name = web_trim_copy(line.substring(0, p1));
  if (p2 < 0) {
    item.url = web_trim_copy(line.substring(p1 + 1));
  } else {
    item.url = web_trim_copy(line.substring(p1 + 1, p2));
    if (p3 < 0) {
      item.format = web_trim_copy(line.substring(p2 + 1));
    } else {
      item.format = web_trim_copy(line.substring(p2 + 1, p3));
      if (p4 < 0) {
        item.region = web_trim_copy(line.substring(p3 + 1));
      } else {
        item.region = web_trim_copy(line.substring(p3 + 1, p4));
        item.logo = web_trim_copy(line.substring(p4 + 1));
      }
    }
  }

  String item_error;
  if (!web_radio_item_valid(item, &item_error)) {
    if (reason) *reason = item_error;
    return WebRadioLineParseResult::Invalid;
  }
  return WebRadioLineParseResult::Item;
}

using WebRadioReadLineCallback = bool (*)(void* context, String& line);

static bool web_parse_radio_config_lines(WebRadioReadLineCallback read_line,
                                         void* read_context,
                                         size_t source_size,
                                         std::vector<WebRadioConfigItem>& items,
                                         String* error,
                                         String* warning = nullptr)
{
  items.clear();
  if (error) error->remove(0);
  if (warning) warning->remove(0);
  if (!read_line || source_size > kWebRadioConfigMaxBytes) {
    return web_radio_set_error(error, "电台配置文件过大");
  }

  WebRadioParseStats stats{};
  String line;
  size_t line_number = 0;
  while (read_line(read_context, line)) {
    ++line_number;
    WebRadioConfigItem item{};
    String reason;
    const WebRadioLineParseResult result =
        web_parse_radio_config_line(line, item, &reason);
    if (result == WebRadioLineParseResult::Skip) continue;

    ++stats.data_lines;
    if (result == WebRadioLineParseResult::Invalid) {
      ++stats.skipped_lines;
      if (stats.first_skipped_line == 0) {
        stats.first_skipped_line = line_number;
        stats.first_skip_reason = reason;
      }
      LOGW("[网页] 跳过不兼容电台配置行：行=%u 原因=%s",
           (unsigned)line_number,
           reason.c_str());
      continue;
    }

    if (items.size() >= kWebRadioMaxItems) {
      return web_radio_set_error(
          error,
          String("有效电台数量超过") + String((unsigned)kWebRadioMaxItems) + "个");
    }
    items.push_back(item);
  }

  if (stats.data_lines > 0 && items.empty()) {
    String message = "未读取到有效电台";
    if (stats.first_skipped_line > 0) {
      message += String("；第") + String((unsigned)stats.first_skipped_line) +
                 "行：" + stats.first_skip_reason;
    }
    return web_radio_set_error(error, message);
  }

  if (warning && stats.skipped_lines > 0) {
    *warning = String("已跳过") + String((unsigned)stats.skipped_lines) +
               "行不兼容内容";
    if (stats.first_skipped_line > 0) {
      *warning += String("；首个为第") +
                  String((unsigned)stats.first_skipped_line) +
                  "行：" + stats.first_skip_reason;
    }
  }
  return true;
}

struct WebRadioMemoryReader {
  const uint8_t* data = nullptr;
  size_t size = 0;
  size_t offset = 0;
};

static bool web_radio_read_memory_line(void* context, String& line)
{
  WebRadioMemoryReader* reader = static_cast<WebRadioMemoryReader*>(context);
  if (!reader || reader->offset >= reader->size) return false;

  const size_t start = reader->offset;
  while (reader->offset < reader->size && reader->data[reader->offset] != '\n') {
    ++reader->offset;
  }
  line.remove(0);
  line.reserve(reader->offset - start + 1);
  for (size_t i = start; i < reader->offset; ++i) {
    line += static_cast<char>(reader->data[i]);
  }
  if (reader->offset < reader->size && reader->data[reader->offset] == '\n') {
    ++reader->offset;
  }
  return true;
}

static bool web_radio_read_file_line(void* context, String& line)
{
  File32* file = static_cast<File32*>(context);
  if (!file || !file->available()) return false;
  line = file->readStringUntil('\n');
  return true;
}

static bool web_parse_radio_config_memory(const uint8_t* data,
                                          size_t size,
                                          std::vector<WebRadioConfigItem>& items,
                                          String* error,
                                          String* warning = nullptr)
{
  WebRadioMemoryReader reader{};
  reader.data = data;
  reader.size = size;
  return web_parse_radio_config_lines(web_radio_read_memory_line,
                                      &reader,
                                      size,
                                      items,
                                      error,
                                      warning);
}

static bool web_validate_radio_config_file(File32& file, void*)
{
  std::vector<WebRadioConfigItem> items;
  String error;
  return web_parse_radio_config_lines(web_radio_read_file_line,
                                      &file,
                                      file.fileSize(),
                                      items,
                                      &error);
}

static bool web_load_radio_config(std::vector<WebRadioConfigItem>& items,
                                  String* error = nullptr,
                                  bool* out_pending = nullptr,
                                  String* warning = nullptr)
{
  items.clear();
  if (error) error->remove(0);
  if (warning) warning->remove(0);
  if (out_pending) *out_pending = false;

  const size_t pending_size =
      storage_config_pending_size(SystemPaths::kRadioList);
  if (pending_size > 0 && pending_size <= kWebRadioConfigMaxBytes) {
    uint8_t* pending = static_cast<uint8_t*>(heap_caps_malloc(
        pending_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!pending) {
      return web_radio_set_error(error, "读取PSRAM待写电台配置失败");
    }
    size_t copied = 0;
    const bool read_ok = storage_config_read_pending(SystemPaths::kRadioList,
                                                     pending,
                                                     pending_size,
                                                     &copied);
    const bool parse_ok = read_ok &&
        web_parse_radio_config_memory(pending, copied, items, error, warning);
    heap_caps_free(pending);
    if (parse_ok && out_pending) *out_pending = true;
    return parse_ok;
  }

  (void)storage_config_recover(SystemPaths::kRadioList,
                               web_validate_radio_config_file,
                               nullptr,
                               3000);

  StorageSdLockGuard guard(2000);
  if (!guard) return web_radio_set_error(error, "等待SD锁超时");

  File32 file = sd.open(SystemPaths::kRadioList, O_RDONLY);
  if (!file) return web_radio_set_error(error, "电台列表文件不存在");

  const bool ok = web_parse_radio_config_lines(web_radio_read_file_line,
                                               &file,
                                               file.fileSize(),
                                               items,
                                               error,
                                               warning);
  file.close();
  return ok;
}

static bool web_serialize_radio_config(
    const std::vector<WebRadioConfigItem>& items,
    String& output)
{
  output.remove(0);
  output.reserve(160 + items.size() * 240);
  output += "# ESP32S3 Player radio list\r\n";
  output += "# 名称|地址|格式|地区|台标；网页保存时先暂存PSRAM，安全窗口再写入TF卡\r\n";
  for (const WebRadioConfigItem& item : items) {
    output += item.name;
    output += '|';
    output += item.url;
    output += '|';
    output += item.format;
    output += '|';
    output += item.region;
    output += '|';
    output += item.logo;
    output += "\r\n";
  }
  return output.length() > 0 && output.length() <= kWebRadioConfigMaxBytes;
}

static void web_radio_config_commit_complete(const char* final_path,
                                             bool success,
                                             void*)
{
  if (!success) {
    LOGW("[网页] 电台配置本次落盘失败，将在下一个安全窗口重试：%s",
         final_path ? final_path : "-");
    return;
  }

  const PlayerSourceState before = player_source_get();
  if (radio_catalog_load()) {
    if (before.type == PlayerSourceType::NET_RADIO) {
      int remapped = -1;
      const RadioItem* matched = nullptr;
      for (size_t i = 0; i < radio_catalog_count(); ++i) {
        const RadioItem* item = radio_catalog_get(i);
        if (!item || !item->valid) continue;
        if ((!before.radio_url.isEmpty() && item->url == before.radio_url) ||
            (before.radio_url.isEmpty() && item->name == before.radio_name)) {
          remapped = static_cast<int>(i);
          matched = item;
          break;
        }
      }
      player_source_remap_radio_catalog(remapped, matched);
      if (remapped >= 0) {
        ui_set_track_pos(remapped, static_cast<int>(radio_catalog_count()));
      }
      LOGI("[网页] 当前电台目录索引已重映射：旧=%d 新=%d",
           before.radio_idx,
           remapped);
    }

    LOGI("[网页] 电台配置已落盘并重新加载：数量=%u",
         (unsigned)radio_catalog_count());
  } else {
    LOGE("[网页] 电台配置落盘成功，但重新加载失败：%s",
         radio_catalog_error().c_str());
  }
  quick_menu_request_refresh();
}

static String web_radio_arg_name(size_t index, const char* field)
{
  char name[40] = {};
  snprintf(name, sizeof(name), "%s_%u", field, (unsigned)index);
  return String(name);
}

static String web_radio_arg(size_t index, const char* field)
{
  return s_server.arg(web_radio_arg_name(index, field));
}

static bool web_parse_radio_save_request(
    std::vector<WebRadioConfigItem>& items,
    String& error)
{
  items.clear();
  error.remove(0);
  if (!s_server.hasArg("count")) {
    error = "缺少电台数量";
    return false;
  }

  const int count = s_server.arg("count").toInt();
  if (count < 0 || count > static_cast<int>(kWebRadioMaxItems)) {
    error = "电台数量必须在0到64之间";
    return false;
  }

  items.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    WebRadioConfigItem item{};
    item.name = web_trim_copy(web_radio_arg(i, "name"));
    item.url = web_trim_copy(web_radio_arg(i, "url"));
    item.format = web_trim_copy(web_radio_arg(i, "format"));
    item.region = web_trim_copy(web_radio_arg(i, "region"));
    item.logo = web_trim_copy(web_radio_arg(i, "logo"));

    String item_error;
    if (!web_radio_item_valid(item, &item_error)) {
      error = String("第") + String(i + 1) + "个电台：" + item_error;
      return false;
    }
    items.push_back(item);
  }
  return true;
}

static bool web_stage_radio_config_from_request(String& error, bool& deferred)
{
  deferred = false;
  std::vector<WebRadioConfigItem> items;
  if (!web_parse_radio_save_request(items, error)) return false;

  String serialized;
  if (!web_serialize_radio_config(items, serialized)) {
    error = "电台配置序列化失败或超过48KB";
    return false;
  }

  if (!storage_config_stage_psram(
          SystemPaths::kRadioList,
          reinterpret_cast<const uint8_t*>(serialized.c_str()),
          serialized.length(),
          web_validate_radio_config_file,
          nullptr,
          web_radio_config_commit_complete,
          nullptr)) {
    error = "电台配置暂存PSRAM失败，原配置未改变";
    return false;
  }

  const bool local_audio_open =
      player_source_type_get() == PlayerSourceType::LOCAL_TRACK &&
      (audio_service_is_playing() || audio_service_is_paused());
  if (local_audio_open) {
    deferred = true;
  } else {
    (void)storage_config_commit_pending(4000);
    deferred = storage_config_has_pending_path(SystemPaths::kRadioList);
  }

  LOGI("[网页] 电台配置已%s：数量=%u",
       deferred ? "暂存PSRAM等待安全窗口" : "写入TF卡",
       (unsigned)items.size());
  return true;
}

static void web_handle_radio_config_get()
{
  if (!storage_is_ready()) {
    web_send_json_err("TF卡未就绪，无法读取电台配置", 503);
    return;
  }

  std::vector<WebRadioConfigItem> items;
  String error;
  String warning;
  bool pending = false;
  const bool ok = web_load_radio_config(items, &error, &pending, &warning);

  web_send_no_cache_headers();
  s_server.sendHeader("Connection", "close");
  s_server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  s_server.send(200, "application/json; charset=utf-8", "{");

  String head;
  head.reserve(256);
  head += "\"ok\":";
  head += ok ? "true" : "false";
  head += ",\"path\":\"";
  head += web_json_escape(String(SystemPaths::kRadioList));
  head += "\",\"asset_dir\":\"";
  head += web_json_escape(String(SystemPaths::kRadioAssetsDir));
  head += "\",\"write_pending\":";
  head += pending ? "true" : "false";
  head += ",\"max_items\":";
  head += String((unsigned)kWebRadioMaxItems);
  head += ",\"error\":\"";
  head += web_json_escape(error);
  head += "\",\"warning\":\"";
  head += web_json_escape(warning);
  head += "\",\"items\":[";
  if (!web_send_chunk(head)) return;

  String chunk;
  chunk.reserve(1400);
  for (size_t i = 0; i < items.size(); ++i) {
    const WebRadioConfigItem& item = items[i];
    if (i) chunk += ',';
    chunk += "{\"index\":";
    chunk += String((unsigned)i);
    chunk += ",\"name\":\"";
    chunk += web_json_escape(item.name);
    chunk += "\",\"url\":\"";
    chunk += web_json_escape(item.url);
    chunk += "\",\"format\":\"";
    chunk += web_json_escape(item.format);
    chunk += "\",\"region\":\"";
    chunk += web_json_escape(item.region);
    chunk += "\",\"logo\":\"";
    chunk += web_json_escape(item.logo);
    chunk += "\"}";
    if (chunk.length() >= 1200 && !web_flush_chunk_buffer(chunk)) return;
  }
  if (!web_flush_chunk_buffer(chunk)) return;
  if (!web_send_chunk("]}")) return;
  web_end_stream_response();
}

static void web_handle_radio_config_save()
{
  if (!storage_is_ready()) {
    web_send_json_err("TF卡未就绪，无法保存电台配置", 503);
    return;
  }

  String error;
  bool deferred = false;
  if (!web_stage_radio_config_from_request(error, deferred)) {
    web_send_json_err(error.c_str(), 400);
    return;
  }

  String json = "{\"ok\":true,\"state\":\"";
  json += deferred ? "pending" : "committed";
  json += "\",\"message\":\"";
  json += deferred
      ? "电台配置已暂存PSRAM，下次切歌后写入TF卡"
      : "电台配置已写入TF卡并重新加载";
  json += "\"}";
  web_send_no_cache_headers();
  s_server.send(200, "application/json; charset=utf-8", json);
}

static bool web_nas_set_error(String* error, const String& message)
{
  if (error) *error = message;
  return false;
}

static void web_nas_append_warning(String* warning, const String& message)
{
  if (!warning || message.isEmpty()) return;
  if (warning->length()) *warning += "；";
  *warning += message;
}

static bool web_nas_text_valid(const String& value,
                               size_t max_bytes,
                               bool allow_empty)
{
  if ((!allow_empty && value.isEmpty()) || value.length() > max_bytes) {
    return false;
  }
  for (size_t i = 0; i < value.length(); ++i) {
    const uint8_t ch = static_cast<uint8_t>(value[i]);
    if (ch == 0 || ch == '\r' || ch == '\n' || ch < 0x20 || ch == '|') {
      return false;
    }
  }
  return true;
}

static bool web_nas_has_http_scheme(const String& value)
{
  if (value.length() < 7) return false;
  String prefix = value.substring(0, value.length() >= 8 ? 8 : 7);
  prefix.toLowerCase();
  return prefix.startsWith("http://") || prefix.startsWith("https://");
}

static String web_nas_normalize_base_url(const String& raw)
{
  String value = web_trim_copy(raw);
  if (value.length() && !value.endsWith("/")) value += '/';
  return value;
}

static String web_nas_normalize_path(const String& raw)
{
  String value = web_trim_copy(raw);
  value.replace('\\', '/');
  while (value.startsWith("./")) value.remove(0, 2);
  while (value.startsWith("/")) value.remove(0, 1);
  while (value.endsWith("/")) value.remove(value.length() - 1);
  return value;
}

static bool web_nas_relative_path_safe(const String& value)
{
  if (value == ".." || value.startsWith("../") ||
      value.indexOf("/../") >= 0 || value.endsWith("/..")) {
    return false;
  }
  return true;
}

static bool web_nas_base_url_valid(const String& value, String* error)
{
  if (!web_nas_text_valid(value, kWebNasMaxBaseUrlBytes, false) ||
      !web_nas_has_http_scheme(value)) {
    return web_nas_set_error(
        error,
        "NAS根地址必须以http://或https://开头，最多768字节");
  }
  return true;
}

static bool web_nas_source_valid(const WebNasConfigSource& source,
                                 String* error)
{
  if (!web_nas_text_valid(source.name, kWebNasMaxNameBytes, false)) {
    return web_nas_set_error(error,
                             "曲库名称不能为空，最多96字节且不能包含竖线");
  }
  if (!web_nas_text_valid(source.relative_path,
                          kWebNasMaxRelativePathBytes,
                          true) ||
      !web_nas_relative_path_safe(source.relative_path)) {
    return web_nas_set_error(error,
                             "相对目录最多384字节，且不能包含..、竖线或换行");
  }
  if (!web_nas_text_valid(source.list_name,
                          kWebNasMaxListNameBytes,
                          false) ||
      !web_nas_relative_path_safe(source.list_name)) {
    return web_nas_set_error(error,
                             "列表文件名不能为空，最多192字节，且不能包含..、竖线或换行");
  }
  return true;
}

static void web_nas_strip_utf8_bom(String& line)
{
  if (line.length() >= 3 &&
      static_cast<uint8_t>(line[0]) == 0xEF &&
      static_cast<uint8_t>(line[1]) == 0xBB &&
      static_cast<uint8_t>(line[2]) == 0xBF) {
    line.remove(0, 3);
  }
}

static bool web_nas_parse_base_line(const String& raw, String& base_url)
{
  String line = web_trim_copy(raw);
  web_nas_strip_utf8_bom(line);
  line.trim();
  if (line.isEmpty() || line.startsWith("#") || line.startsWith(";")) {
    return false;
  }
  base_url = web_nas_normalize_base_url(line);
  return true;
}

static bool web_nas_parse_source_line(const String& raw,
                                      WebNasConfigSource& source,
                                      String* reason)
{
  String line = web_trim_copy(raw);
  web_nas_strip_utf8_bom(line);
  line.trim();
  if (line.isEmpty() || line.startsWith("#") || line.startsWith(";")) {
    return false;
  }

  const int p1 = line.indexOf('|');
  const int p2 = p1 >= 0 ? line.indexOf('|', p1 + 1) : -1;

  source = WebNasConfigSource{};
  if (p1 < 0) {
    // 兼容旧版单字段：名称和相对目录相同。
    source.name = line;
    source.relative_path = line;
  } else {
    source.name = web_trim_copy(line.substring(0, p1));
    source.relative_path = p2 < 0
        ? web_trim_copy(line.substring(p1 + 1))
        : web_trim_copy(line.substring(p1 + 1, p2));
    if (p2 >= 0) {
      source.list_name = web_trim_copy(line.substring(p2 + 1));
    }
  }

  source.relative_path = web_nas_normalize_path(source.relative_path);
  source.list_name = web_nas_normalize_path(source.list_name);
  if (source.name.isEmpty()) {
    source.name = source.relative_path.length()
        ? source.relative_path
        : String("NAS音乐");
  }
  if (source.list_name.isEmpty()) source.list_name = "net_music.txt";

  String item_error;
  if (!web_nas_source_valid(source, &item_error)) {
    if (reason) *reason = item_error;
    return false;
  }
  return true;
}

static bool web_nas_read_pending_text(const char* path,
                                      size_t max_bytes,
                                      String& text)
{
  text.remove(0);
  const size_t pending_size = storage_config_pending_size(path);
  if (pending_size == 0 || pending_size > max_bytes) return false;

  uint8_t* pending = static_cast<uint8_t*>(heap_caps_malloc(
      pending_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!pending) return false;

  size_t copied = 0;
  const bool read_ok = storage_config_read_pending(path,
                                                   pending,
                                                   pending_size,
                                                   &copied);
  bool copy_ok = false;
  if (read_ok && copied == pending_size && text.reserve(copied + 1)) {
    for (size_t i = 0; i < copied; ++i) text += static_cast<char>(pending[i]);
    copy_ok = true;
  }
  heap_caps_free(pending);
  return copy_ok;
}

static bool web_nas_parse_base_text(const String& text,
                                    String& base_url,
                                    String* error)
{
  base_url.remove(0);
  size_t start = 0;
  while (start <= text.length()) {
    int end = text.indexOf('\n', start);
    if (end < 0) end = static_cast<int>(text.length());
    String line = text.substring(start, static_cast<size_t>(end));
    if (web_nas_parse_base_line(line, base_url)) break;
    if (static_cast<size_t>(end) >= text.length()) break;
    start = static_cast<size_t>(end) + 1;
  }

  if (base_url.isEmpty()) {
    return web_nas_set_error(error, "NAS根地址配置为空");
  }
  return web_nas_base_url_valid(base_url, error);
}

static bool web_nas_parse_sources_text(
    const String& text,
    std::vector<WebNasConfigSource>& sources,
    String* error,
    String* warning)
{
  sources.clear();
  size_t skipped = 0;
  size_t first_skipped_line = 0;
  String first_reason;
  size_t start = 0;
  size_t line_number = 0;

  while (start <= text.length()) {
    int end = text.indexOf('\n', start);
    if (end < 0) end = static_cast<int>(text.length());
    ++line_number;
    String raw = text.substring(start, static_cast<size_t>(end));
    String trimmed = web_trim_copy(raw);
    web_nas_strip_utf8_bom(trimmed);
    trimmed.trim();

    if (trimmed.length() && !trimmed.startsWith("#") &&
        !trimmed.startsWith(";")) {
      WebNasConfigSource source{};
      String reason;
      if (web_nas_parse_source_line(raw, source, &reason)) {
        if (sources.size() >= kWebNasMaxSources) {
          ++skipped;
          if (first_skipped_line == 0) {
            first_skipped_line = line_number;
            first_reason = "超过最多8个曲库源，后续条目已忽略";
          }
        } else {
          sources.push_back(source);
        }
      } else {
        ++skipped;
        if (first_skipped_line == 0) {
          first_skipped_line = line_number;
          first_reason = reason.length() ? reason : String("格式无效");
        }
      }
    }

    if (static_cast<size_t>(end) >= text.length()) break;
    start = static_cast<size_t>(end) + 1;
  }

  if (sources.empty()) {
    return web_nas_set_error(error, "NAS曲库源配置没有有效条目");
  }
  if (skipped > 0 && warning) {
    *warning = String("已跳过") + String((unsigned)skipped) +
        "行不兼容曲库源；首个为第" +
        String((unsigned)first_skipped_line) + "行：" + first_reason;
  }
  return true;
}

static bool web_nas_read_file_text(const char* path,
                                   size_t max_bytes,
                                   String& text,
                                   bool* missing,
                                   String* error)
{
  text.remove(0);
  if (missing) *missing = false;

  StorageSdLockGuard guard(2500);
  if (!guard) return web_nas_set_error(error, "等待SD锁超时");

  File32 file = sd.open(path, O_RDONLY);
  if (!file) {
    if (missing) *missing = true;
    return true;
  }

  const uint64_t size64 = file.fileSize();
  if (size64 > max_bytes) {
    file.close();
    return web_nas_set_error(error, "NAS配置文件过大");
  }

  const size_t size = static_cast<size_t>(size64);
  if (!text.reserve(size + 1)) {
    file.close();
    return web_nas_set_error(error, "读取NAS配置时内存不足");
  }

  while (file.available()) {
    const int value = file.read();
    if (value < 0) break;
    text += static_cast<char>(value);
  }
  file.close();
  return true;
}

static bool web_validate_nas_base_file(File32& file, void*)
{
  const uint64_t size64 = file.fileSize();
  if (size64 == 0 || size64 > kWebNasBaseConfigMaxBytes) return false;

  String text;
  if (!text.reserve(static_cast<size_t>(size64) + 1)) return false;
  while (file.available()) {
    const int value = file.read();
    if (value < 0) break;
    text += static_cast<char>(value);
  }
  String base_url;
  return web_nas_parse_base_text(text, base_url, nullptr);
}

static bool web_validate_nas_sources_file(File32& file, void*)
{
  const uint64_t size64 = file.fileSize();
  if (size64 == 0 || size64 > kWebNasSourcesConfigMaxBytes) return false;

  String text;
  if (!text.reserve(static_cast<size_t>(size64) + 1)) return false;
  while (file.available()) {
    const int value = file.read();
    if (value < 0) break;
    text += static_cast<char>(value);
  }
  std::vector<WebNasConfigSource> sources;
  return web_nas_parse_sources_text(text, sources, nullptr, nullptr);
}

static bool web_load_nas_config(String& base_url,
                                std::vector<WebNasConfigSource>& sources,
                                String* error,
                                String* warning,
                                bool* base_pending,
                                bool* sources_pending)
{
  base_url.remove(0);
  sources.clear();
  if (error) error->remove(0);
  if (warning) warning->remove(0);
  if (base_pending) *base_pending = false;
  if (sources_pending) *sources_pending = false;

  String base_text;
  const bool have_pending_base = web_nas_read_pending_text(
      SystemPaths::kNetMusicBase, kWebNasBaseConfigMaxBytes, base_text);
  if (have_pending_base) {
    if (base_pending) *base_pending = true;
  } else {
    (void)storage_config_recover(SystemPaths::kNetMusicBase,
                                 web_validate_nas_base_file,
                                 nullptr,
                                 2500);
    bool missing = false;
    String read_error;
    if (!web_nas_read_file_text(SystemPaths::kNetMusicBase,
                                kWebNasBaseConfigMaxBytes,
                                base_text,
                                &missing,
                                &read_error)) {
      return web_nas_set_error(error, read_error);
    }
    if (missing) {
      web_nas_append_warning(warning, "NAS根地址配置不存在，请填写后保存");
    }
  }

  if (base_text.length()) {
    String parse_error;
    if (!web_nas_parse_base_text(base_text, base_url, &parse_error)) {
      return web_nas_set_error(error, parse_error);
    }
  }

  String sources_text;
  const bool have_pending_sources = web_nas_read_pending_text(
      SystemPaths::kNetMusicSources,
      kWebNasSourcesConfigMaxBytes,
      sources_text);
  if (have_pending_sources) {
    if (sources_pending) *sources_pending = true;
  } else {
    (void)storage_config_recover(SystemPaths::kNetMusicSources,
                                 web_validate_nas_sources_file,
                                 nullptr,
                                 2500);
    bool missing = false;
    String read_error;
    if (!web_nas_read_file_text(SystemPaths::kNetMusicSources,
                                kWebNasSourcesConfigMaxBytes,
                                sources_text,
                                &missing,
                                &read_error)) {
      return web_nas_set_error(error, read_error);
    }
    if (missing) {
      web_nas_append_warning(
          warning,
          "曲库源配置不存在，当前显示兼容旧版的根目录曲库");
    }
  }

  if (sources_text.length()) {
    String parse_error;
    String parse_warning;
    if (!web_nas_parse_sources_text(sources_text,
                                    sources,
                                    &parse_error,
                                    &parse_warning)) {
      return web_nas_set_error(error, parse_error);
    }
    web_nas_append_warning(warning, parse_warning);
  } else {
    WebNasConfigSource source{};
    source.name = "NAS音乐";
    source.relative_path = "";
    source.list_name = "net_music.txt";
    sources.push_back(source);
  }
  return true;
}

static bool web_serialize_nas_base(const String& base_url, String& output)
{
  output.remove(0);
  output.reserve(base_url.length() + 160);
  output += "# ESP32S3 Player NAS base URL\r\n";
  output += "# 网页保存时先暂存PSRAM，安全窗口再写入TF卡\r\n";
  output += base_url;
  output += "\r\n";
  return output.length() <= kWebNasBaseConfigMaxBytes;
}

static bool web_serialize_nas_sources(
    const std::vector<WebNasConfigSource>& sources,
    String& output)
{
  output.remove(0);
  output.reserve(180 + sources.size() * 360);
  output += "# ESP32S3 Player NAS sources\r\n";
  output += "# 名称|相对目录|列表文件名\r\n";
  for (const WebNasConfigSource& source : sources) {
    output += source.name;
    output += '|';
    output += source.relative_path;
    output += '|';
    output += source.list_name;
    output += "\r\n";
  }
  return output.length() <= kWebNasSourcesConfigMaxBytes;
}

static String web_nas_arg_name(size_t index, const char* field)
{
  char name[40] = {};
  snprintf(name, sizeof(name), "%s_%u", field, (unsigned)index);
  return String(name);
}

static String web_nas_arg(size_t index, const char* field)
{
  return s_server.arg(web_nas_arg_name(index, field));
}

static bool web_parse_nas_save_request(
    String& base_url,
    std::vector<WebNasConfigSource>& sources,
    uint8_t& active_index,
    String& error)
{
  error.remove(0);
  base_url = web_nas_normalize_base_url(s_server.arg("base_url"));
  if (!web_nas_base_url_valid(base_url, &error)) return false;

  if (!s_server.hasArg("count")) {
    error = "缺少NAS曲库源数量";
    return false;
  }
  const int count = s_server.arg("count").toInt();
  if (count <= 0 || count > static_cast<int>(kWebNasMaxSources)) {
    error = "NAS曲库源数量必须在1到8之间";
    return false;
  }

  const int requested_active = s_server.arg("active_index").toInt();
  if (requested_active < 0 || requested_active >= count) {
    error = "当前NAS曲库源索引无效";
    return false;
  }
  active_index = static_cast<uint8_t>(requested_active);

  sources.clear();
  sources.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    WebNasConfigSource source{};
    source.name = web_trim_copy(web_nas_arg(i, "name"));
    source.relative_path = web_nas_normalize_path(web_nas_arg(i, "path"));
    source.list_name = web_nas_normalize_path(web_nas_arg(i, "list"));
    if (source.list_name.isEmpty()) source.list_name = "net_music.txt";

    String source_error;
    if (!web_nas_source_valid(source, &source_error)) {
      error = String("第") + String(i + 1) + "个NAS曲库源：" + source_error;
      return false;
    }
    sources.push_back(source);
  }
  return true;
}

static uint16_t web_nas_reset_commit_state(bool apply_after_commit,
                                           uint8_t requested_source_index)
{
  portENTER_CRITICAL(&s_nas_config_apply_mux);
  ++s_nas_config_commit_generation;
  if (s_nas_config_commit_generation == 0) {
    ++s_nas_config_commit_generation;
  }
  s_nas_config_apply_requested = apply_after_commit;
  s_nas_config_apply_pending = false;
  s_nas_config_apply_at_ms = 0;
  s_nas_config_commit_success_mask = 0;
  s_nas_config_requested_source_index = requested_source_index;
  const uint16_t generation = s_nas_config_commit_generation;
  portEXIT_CRITICAL(&s_nas_config_apply_mux);
  return generation;
}

static void* web_nas_commit_context(uint16_t generation, uint8_t bit)
{
  const uintptr_t token =
      (static_cast<uintptr_t>(generation) << 8) | bit;
  return reinterpret_cast<void*>(token);
}

static bool web_nas_apply_is_pending(uint8_t* requested_source_index = nullptr)
{
  portENTER_CRITICAL(&s_nas_config_apply_mux);
  const bool pending = s_nas_config_apply_requested ||
                       s_nas_config_apply_pending;
  if (requested_source_index) {
    *requested_source_index = s_nas_config_requested_source_index;
  }
  portEXIT_CRITICAL(&s_nas_config_apply_mux);
  return pending;
}

static void web_nas_config_commit_complete(const char* final_path,
                                           bool success,
                                           void* context)
{
  const uintptr_t token = reinterpret_cast<uintptr_t>(context);
  const uint8_t bit = static_cast<uint8_t>(token & 0xFFu);
  const uint16_t generation = static_cast<uint16_t>(token >> 8);
  if ((bit != kWebNasCommitBaseBit && bit != kWebNasCommitSourcesBit) ||
      generation == 0) {
    return;
  }

  bool current_generation = false;
  uint16_t current_generation_value = 0;
  portENTER_CRITICAL(&s_nas_config_apply_mux);
  current_generation_value = s_nas_config_commit_generation;
  current_generation = generation == current_generation_value;
  portEXIT_CRITICAL(&s_nas_config_apply_mux);
  if (!current_generation) {
    LOGD("[网页] 忽略旧NAS配置事务回调：世代=%u 当前=%u 路径=%s",
         (unsigned)generation,
         (unsigned)current_generation_value,
         final_path ? final_path : "-");
    return;
  }

  if (!success) {
    portENTER_CRITICAL(&s_nas_config_apply_mux);
    s_nas_config_commit_success_mask &= static_cast<uint8_t>(~bit);
    portEXIT_CRITICAL(&s_nas_config_apply_mux);
    LOGW("[网页] NAS配置本次未完成落盘：%s", final_path);
    return;
  }

  bool schedule_apply = false;
  portENTER_CRITICAL(&s_nas_config_apply_mux);
  if (generation == s_nas_config_commit_generation) {
    s_nas_config_commit_success_mask |= bit;
    if (s_nas_config_commit_success_mask == kWebNasCommitAllBits) {
      if (s_nas_config_apply_requested) {
        s_nas_config_apply_pending = true;
        s_nas_config_apply_at_ms = millis() + 700;
        schedule_apply = true;
      }
      s_nas_config_apply_requested = false;
    }
  }
  portEXIT_CRITICAL(&s_nas_config_apply_mux);

  LOGI("[网页] NAS配置文件已落盘：%s", final_path);
  if (schedule_apply) {
    LOGI("[网页] NAS配置完整落盘，已安排延迟应用");
  }
}

static void web_apply_saved_nas_config_now()
{
  uint8_t requested_index = 0;
  portENTER_CRITICAL(&s_nas_config_apply_mux);
  requested_index = s_nas_config_requested_source_index;
  portEXIT_CRITICAL(&s_nas_config_apply_mux);

  const PlayerSourceState before = player_source_get();
  if (before.type == PlayerSourceType::NET_TRACK) {
    player_stop_net_track();
  }

  net_music_catalog_clear();
  if (!net_music_catalog_load_base()) {
    LOGE("[网页] NAS配置应用失败：%s",
         net_music_catalog_error().c_str());
    ui_show_notice_popup("NAS配置应用失败", "请检查根地址和曲库源配置");
    quick_menu_request_refresh();
    return;
  }

  const uint8_t count = net_music_catalog_source_count();
  if (count == 0) {
    LOGE("[网页] NAS配置应用失败：没有有效曲库源");
    ui_show_notice_popup("NAS配置应用失败", "没有有效曲库源");
    quick_menu_request_refresh();
    return;
  }
  if (requested_index >= count) requested_index = 0;

  if (!net_music_catalog_select_source(requested_index)) {
    LOGE("[网页] NAS配置应用失败：无法选择曲库源=%u",
         (unsigned)requested_index);
    ui_show_notice_popup("NAS配置应用失败", "无法选择当前曲库源");
    quick_menu_request_refresh();
    return;
  }

  (void)player_snapshot_reload_net_context_for_active_source();
  LOGI("[网页] NAS配置已应用：base=%s 源=%u/%u 名称=%s",
       net_music_catalog_base_url().c_str(),
       (unsigned)requested_index,
       (unsigned)count,
       net_music_catalog_active_source_name().c_str());
  ui_show_notice_popup("NAS配置已应用", "进入NAS页后重新加载歌曲列表");
  quick_menu_request_refresh();
}

static void web_process_pending_nas_config_apply()
{
  const uint32_t now = millis();
  bool due = false;

  portENTER_CRITICAL(&s_nas_config_apply_mux);
  if (s_nas_config_apply_pending &&
      static_cast<int32_t>(now - s_nas_config_apply_at_ms) >= 0) {
    s_nas_config_apply_pending = false;
    due = true;
  }
  portEXIT_CRITICAL(&s_nas_config_apply_mux);

  if (due) web_apply_saved_nas_config_now();
}

static bool web_stage_nas_config_from_request(bool apply_after_commit,
                                              String& error,
                                              bool& deferred)
{
  deferred = false;
  String base_url;
  std::vector<WebNasConfigSource> sources;
  uint8_t active_index = 0;
  if (!web_parse_nas_save_request(base_url,
                                  sources,
                                  active_index,
                                  error)) {
    return false;
  }

  String base_serialized;
  String sources_serialized;
  if (!web_serialize_nas_base(base_url, base_serialized) ||
      !web_serialize_nas_sources(sources, sources_serialized)) {
    error = "NAS配置序列化失败或超过大小限制";
    return false;
  }

  const uint16_t generation =
      web_nas_reset_commit_state(apply_after_commit, active_index);

  // 先暂存根地址，再暂存曲库源；任意一步失败都撤销另一部分，
  // 避免只留下半套PSRAM待写配置。
  if (!storage_config_stage_psram(
          SystemPaths::kNetMusicBase,
          reinterpret_cast<const uint8_t*>(base_serialized.c_str()),
          base_serialized.length(),
          web_validate_nas_base_file,
          nullptr,
          web_nas_config_commit_complete,
          web_nas_commit_context(generation, kWebNasCommitBaseBit))) {
    error = "NAS根地址暂存PSRAM失败，原配置未改变";
    (void)web_nas_reset_commit_state(false, 0);
    return false;
  }

  if (!storage_config_stage_psram(
          SystemPaths::kNetMusicSources,
          reinterpret_cast<const uint8_t*>(sources_serialized.c_str()),
          sources_serialized.length(),
          web_validate_nas_sources_file,
          nullptr,
          web_nas_config_commit_complete,
          web_nas_commit_context(generation, kWebNasCommitSourcesBit))) {
    // 先推进世代，使回滚时触发的旧回调不会污染新的事务状态。
    (void)web_nas_reset_commit_state(false, 0);
    (void)storage_config_discard_pending_path(
        SystemPaths::kNetMusicBase,
        "NAS曲库源暂存失败，回滚根地址待写项");
    error = "NAS曲库源暂存PSRAM失败，原配置未改变";
    return false;
  }

  const bool local_audio_open =
      player_source_type_get() == PlayerSourceType::LOCAL_TRACK &&
      (audio_service_is_playing() || audio_service_is_paused());
  if (local_audio_open) {
    deferred = true;
  } else {
    (void)storage_config_commit_pending(4000);
    deferred = storage_config_has_pending_path(SystemPaths::kNetMusicBase) ||
               storage_config_has_pending_path(SystemPaths::kNetMusicSources);
  }

  LOGI("[网页] NAS配置已%s：base=%s 源=%u 当前=%u 应用=%d",
       deferred ? "暂存PSRAM等待安全窗口" : "写入TF卡",
       base_url.c_str(),
       (unsigned)sources.size(),
       (unsigned)active_index,
       apply_after_commit ? 1 : 0);
  return true;
}

static void web_handle_nas_config_get()
{
  if (!storage_is_ready()) {
    web_send_json_err("TF卡未就绪，无法读取NAS配置", 503);
    return;
  }

  String base_url;
  std::vector<WebNasConfigSource> sources;
  String error;
  String warning;
  bool base_pending = false;
  bool sources_pending = false;
  const bool ok = web_load_nas_config(base_url,
                                      sources,
                                      &error,
                                      &warning,
                                      &base_pending,
                                      &sources_pending);

  uint8_t active_index = net_music_catalog_active_source_index();
  uint8_t requested_apply_index = active_index;
  const bool apply_pending = web_nas_apply_is_pending(&requested_apply_index);
  if (apply_pending) active_index = requested_apply_index;
  if (active_index >= sources.size()) active_index = 0;

  web_send_no_cache_headers();
  s_server.sendHeader("Connection", "close");
  s_server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  s_server.send(200, "application/json; charset=utf-8", "{");

  String head;
  head.reserve(640);
  head += "\"ok\":";
  head += ok ? "true" : "false";
  head += ",\"configured\":";
  head += base_url.length() ? "true" : "false";
  head += ",\"base_path\":\"";
  head += web_json_escape(String(SystemPaths::kNetMusicBase));
  head += "\",\"sources_path\":\"";
  head += web_json_escape(String(SystemPaths::kNetMusicSources));
  head += "\",\"base_url\":\"";
  head += web_json_escape(base_url);
  head += "\",\"base_pending\":";
  head += base_pending ? "true" : "false";
  head += ",\"sources_pending\":";
  head += sources_pending ? "true" : "false";
  head += ",\"write_pending\":";
  head += (base_pending || sources_pending) ? "true" : "false";
  head += ",\"apply_pending\":";
  head += apply_pending ? "true" : "false";
  head += ",\"active_index\":";
  head += String((unsigned)active_index);
  head += ",\"max_sources\":";
  head += String((unsigned)kWebNasMaxSources);
  head += ",\"error\":\"";
  head += web_json_escape(error);
  head += "\",\"warning\":\"";
  head += web_json_escape(warning);
  head += "\",\"sources\":[";
  if (!web_send_chunk(head)) return;

  String chunk;
  chunk.reserve(1200);
  for (size_t i = 0; i < sources.size(); ++i) {
    const WebNasConfigSource& source = sources[i];
    if (i) chunk += ',';
    chunk += "{\"index\":";
    chunk += String((unsigned)i);
    chunk += ",\"name\":\"";
    chunk += web_json_escape(source.name);
    chunk += "\",\"relative_path\":\"";
    chunk += web_json_escape(source.relative_path);
    chunk += "\",\"list_name\":\"";
    chunk += web_json_escape(source.list_name);
    chunk += "\",\"active\":";
    chunk += (i == active_index) ? "true" : "false";
    chunk += '}';
    if (chunk.length() >= 1000 && !web_flush_chunk_buffer(chunk)) return;
  }
  if (!web_flush_chunk_buffer(chunk)) return;
  if (!web_send_chunk("]}")) return;
  web_end_stream_response();
}

static void web_handle_nas_config_save_common(bool apply_after_commit)
{
  if (!storage_is_ready()) {
    web_send_json_err("TF卡未就绪，无法保存NAS配置", 503);
    return;
  }

  String error;
  bool deferred = false;
  if (!web_stage_nas_config_from_request(apply_after_commit,
                                         error,
                                         deferred)) {
    web_send_json_err(error.c_str(), 400);
    return;
  }

  String message;
  if (deferred) {
    message = apply_after_commit
        ? "NAS配置已暂存PSRAM，下次切歌后写入TF卡并应用"
        : "NAS配置已暂存PSRAM，下次切歌后写入TF卡";
  } else {
    message = apply_after_commit
        ? "NAS配置已写入TF卡，即将停止NAS播放并应用新配置"
        : "NAS配置已写入TF卡；当前运行配置保持不变";
  }

  String json = "{\"ok\":true,\"state\":\"";
  json += deferred ? "pending" : "committed";
  json += "\",\"apply\":";
  json += apply_after_commit ? "true" : "false";
  json += ",\"message\":\"";
  json += web_json_escape(message);
  json += "\"}";
  web_send_no_cache_headers();
  s_server.send(200, "application/json; charset=utf-8", json);
}

static void web_handle_nas_config_save()
{
  web_handle_nas_config_save_common(false);
}

static void web_handle_nas_config_apply()
{
  web_handle_nas_config_save_common(true);
}

static bool web_validate_radio_logo_bytes(const uint8_t* data,
                                          size_t size,
                                          const char** extension)
{
  if (extension) *extension = nullptr;
  if (!data || size < 8 || size > kWebRadioLogoMaxBytes) return false;

  const bool jpeg = data[0] == 0xFF && data[1] == 0xD8;
  const bool png = data[0] == 0x89 && data[1] == 0x50 &&
                   data[2] == 0x4E && data[3] == 0x47 &&
                   data[4] == 0x0D && data[5] == 0x0A &&
                   data[6] == 0x1A && data[7] == 0x0A;
  if (jpeg) {
    if (extension) *extension = ".jpg";
    return true;
  }
  if (png) {
    if (extension) *extension = ".png";
    return true;
  }
  return false;
}

static bool web_validate_radio_logo_file(File32& file, void*)
{
  const uint64_t size64 = file.fileSize();
  if (size64 < 8 || size64 > kWebRadioLogoMaxBytes) return false;

  uint8_t head[8] = {};
  if (file.read(head, sizeof(head)) != sizeof(head)) return false;
  const bool jpeg = head[0] == 0xFF && head[1] == 0xD8;
  const bool png = head[0] == 0x89 && head[1] == 0x50 &&
                   head[2] == 0x4E && head[3] == 0x47 &&
                   head[4] == 0x0D && head[5] == 0x0A &&
                   head[6] == 0x1A && head[7] == 0x0A;
  return jpeg || png;
}

static void web_radio_logo_upload_reset()
{
  if (s_radio_logo_upload.data) {
    heap_caps_free(s_radio_logo_upload.data);
  }
  s_radio_logo_upload = WebRadioLogoUploadState{};
}

static void web_radio_logo_upload_fail(const char* message)
{
  s_radio_logo_upload.failed = true;
  snprintf(s_radio_logo_upload.error,
           sizeof(s_radio_logo_upload.error),
           "%s",
           message ? message : "台标上传失败");
}

static bool web_radio_logo_upload_reserve(size_t required)
{
  if (required <= s_radio_logo_upload.capacity) return true;
  if (required > kWebRadioLogoMaxBytes) return false;

  size_t capacity = s_radio_logo_upload.capacity ?
      s_radio_logo_upload.capacity : 16 * 1024;
  while (capacity < required) {
    capacity *= 2;
    if (capacity > kWebRadioLogoMaxBytes) {
      capacity = kWebRadioLogoMaxBytes;
      break;
    }
  }

  void* fresh = heap_caps_realloc(s_radio_logo_upload.data,
                                  capacity,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!fresh) return false;
  s_radio_logo_upload.data = static_cast<uint8_t*>(fresh);
  s_radio_logo_upload.capacity = capacity;
  return true;
}

static void web_radio_logo_commit_complete(const char* final_path,
                                           bool success,
                                           void*)
{
  if (success) {
    LOGI("[网页] 电台台标已在安全窗口写入：%s",
         final_path ? final_path : "-");
  } else {
    LOGW("[网页] 电台台标本次写入失败，将在下一个安全窗口重试：%s",
         final_path ? final_path : "-");
  }
}

static void web_handle_radio_logo_upload_stream()
{
  HTTPUpload& upload = s_server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    web_radio_logo_upload_reset();
    s_radio_logo_upload.slot = s_server.arg("slot").toInt();
    if (s_radio_logo_upload.slot < 0 ||
        s_radio_logo_upload.slot >= static_cast<int>(kWebRadioMaxItems)) {
      web_radio_logo_upload_fail("台标槽位无效");
    }
    return;
  }

  if (upload.status == UPLOAD_FILE_WRITE) {
    if (s_radio_logo_upload.failed) return;
    const size_t required = s_radio_logo_upload.size + upload.currentSize;
    if (!web_radio_logo_upload_reserve(required)) {
      web_radio_logo_upload_fail("台标超过1MB或PSRAM不足");
      return;
    }
    memcpy(s_radio_logo_upload.data + s_radio_logo_upload.size,
           upload.buf,
           upload.currentSize);
    s_radio_logo_upload.size = required;
    return;
  }

  if (upload.status == UPLOAD_FILE_ABORTED) {
    web_radio_logo_upload_fail("台标上传已中止");
    s_radio_logo_upload.result_ready = true;
    return;
  }

  if (upload.status != UPLOAD_FILE_END) return;
  s_radio_logo_upload.result_ready = true;
  if (s_radio_logo_upload.failed) return;

  const char* extension = nullptr;
  if (!web_validate_radio_logo_bytes(s_radio_logo_upload.data,
                                     s_radio_logo_upload.size,
                                     &extension)) {
    web_radio_logo_upload_fail("只支持完整的JPEG或PNG图片，最大1MB");
    return;
  }

  snprintf(s_radio_logo_upload.path,
           sizeof(s_radio_logo_upload.path),
           "%s/radio_%02d%s",
           SystemPaths::kRadioAssetsDir,
           s_radio_logo_upload.slot,
           extension);

  if (!storage_config_stage_psram(
          s_radio_logo_upload.path,
          s_radio_logo_upload.data,
          s_radio_logo_upload.size,
          web_validate_radio_logo_file,
          nullptr,
          web_radio_logo_commit_complete,
          nullptr)) {
    web_radio_logo_upload_fail("台标暂存PSRAM失败，原图片未改变");
    return;
  }

  s_radio_logo_upload.staged = true;
  const bool local_audio_open =
      player_source_type_get() == PlayerSourceType::LOCAL_TRACK &&
      (audio_service_is_playing() || audio_service_is_paused());
  if (local_audio_open) {
    s_radio_logo_upload.deferred = true;
  } else {
    (void)storage_config_commit_pending(5000);
    s_radio_logo_upload.deferred =
        storage_config_has_pending_path(s_radio_logo_upload.path);
  }
}

static void web_handle_radio_logo_upload_complete()
{
  if (!s_radio_logo_upload.result_ready || s_radio_logo_upload.failed ||
      !s_radio_logo_upload.staged) {
    const String message = s_radio_logo_upload.error[0]
        ? String(s_radio_logo_upload.error)
        : String("未收到有效台标文件");
    web_radio_logo_upload_reset();
    web_send_json_err(message.c_str(), 400);
    return;
  }

  String json = "{\"ok\":true,\"state\":\"";
  json += s_radio_logo_upload.deferred ? "pending" : "committed";
  json += "\",\"path\":\"";
  json += web_json_escape(String(s_radio_logo_upload.path));
  json += "\",\"size\":";
  json += String((unsigned)s_radio_logo_upload.size);
  json += ",\"message\":\"";
  json += s_radio_logo_upload.deferred
      ? "台标已暂存PSRAM，下次切歌后写入TF卡"
      : "台标已写入TF卡";
  json += "\"}";

  web_radio_logo_upload_reset();
  web_send_no_cache_headers();
  s_server.send(200, "application/json; charset=utf-8", json);
}

static const char* web_system_asset_path(WebSystemAssetKind kind)
{
  switch (kind) {
    case WebSystemAssetKind::DefaultCover:
      return SystemPaths::kDefaultCover;
    case WebSystemAssetKind::NetCoverLoading:
      return SystemPaths::kNetCoverLoading;
    default:
      return nullptr;
  }
}

static const char* web_system_asset_label(WebSystemAssetKind kind)
{
  switch (kind) {
    case WebSystemAssetKind::DefaultCover:
      return "默认封面";
    case WebSystemAssetKind::NetCoverLoading:
      return "NAS加载图";
    default:
      return "系统图片";
  }
}

static bool web_validate_system_jpeg_bytes(const uint8_t* data, size_t size)
{
  if (!data || size < 4 || size > kWebSystemAssetMaxBytes) return false;
  return data[0] == 0xFF && data[1] == 0xD8 &&
         data[size - 2] == 0xFF && data[size - 1] == 0xD9;
}

static bool web_validate_system_jpeg_file(File32& file, void*)
{
  const uint64_t size64 = file.fileSize();
  if (size64 < 4 || size64 > kWebSystemAssetMaxBytes) return false;

  uint8_t head[2] = {};
  uint8_t tail[2] = {};
  if (!file.seekSet(0) || file.read(head, sizeof(head)) != sizeof(head)) {
    return false;
  }
  if (!file.seekSet(size64 - sizeof(tail)) ||
      file.read(tail, sizeof(tail)) != sizeof(tail)) {
    return false;
  }
  return head[0] == 0xFF && head[1] == 0xD8 &&
         tail[0] == 0xFF && tail[1] == 0xD9;
}

static void web_system_asset_upload_reset()
{
  if (s_system_asset_upload.data) {
    heap_caps_free(s_system_asset_upload.data);
  }
  s_system_asset_upload = WebSystemAssetUploadState{};
}

static void web_system_asset_upload_fail(const char* message)
{
  s_system_asset_upload.failed = true;
  snprintf(s_system_asset_upload.error,
           sizeof(s_system_asset_upload.error),
           "%s",
           message ? message : "系统图片上传失败");
}

static bool web_system_asset_upload_reserve(size_t required)
{
  if (required <= s_system_asset_upload.capacity) return true;
  if (required > kWebSystemAssetMaxBytes) return false;

  size_t capacity = s_system_asset_upload.capacity ?
      s_system_asset_upload.capacity : 16 * 1024;
  while (capacity < required) {
    capacity *= 2;
    if (capacity > kWebSystemAssetMaxBytes) {
      capacity = kWebSystemAssetMaxBytes;
      break;
    }
  }

  void* fresh = heap_caps_realloc(s_system_asset_upload.data,
                                  capacity,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!fresh) return false;
  s_system_asset_upload.data = static_cast<uint8_t*>(fresh);
  s_system_asset_upload.capacity = capacity;
  return true;
}

static void web_system_asset_commit_complete(const char* final_path,
                                             bool success,
                                             void*)
{
  const WebSystemAssetKind kind =
      final_path && strcmp(final_path, SystemPaths::kDefaultCover) == 0
          ? WebSystemAssetKind::DefaultCover
          : WebSystemAssetKind::NetCoverLoading;
  if (success) {
    LOGI("[网页] %s已在安全窗口写入：%s",
         web_system_asset_label(kind),
         final_path ? final_path : "-");
  } else {
    LOGW("[网页] %s本次写入失败，将在下一个安全窗口重试：%s",
         web_system_asset_label(kind),
         final_path ? final_path : "-");
  }
}

static void web_handle_system_asset_upload_stream(WebSystemAssetKind kind)
{
  HTTPUpload& upload = s_server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    web_system_asset_upload_reset();
    s_system_asset_upload.kind = kind;
    return;
  }

  if (s_system_asset_upload.kind != kind) {
    web_system_asset_upload_fail("图片上传状态不一致");
    return;
  }

  if (upload.status == UPLOAD_FILE_WRITE) {
    if (s_system_asset_upload.failed) return;
    const size_t required = s_system_asset_upload.size + upload.currentSize;
    if (!web_system_asset_upload_reserve(required)) {
      web_system_asset_upload_fail("图片超过400KB或PSRAM不足");
      return;
    }
    memcpy(s_system_asset_upload.data + s_system_asset_upload.size,
           upload.buf,
           upload.currentSize);
    s_system_asset_upload.size = required;
    return;
  }

  if (upload.status == UPLOAD_FILE_ABORTED) {
    web_system_asset_upload_fail("图片上传已中止");
    s_system_asset_upload.result_ready = true;
    return;
  }

  if (upload.status != UPLOAD_FILE_END) return;
  s_system_asset_upload.result_ready = true;
  if (s_system_asset_upload.failed) return;

  if (!web_validate_system_jpeg_bytes(s_system_asset_upload.data,
                                      s_system_asset_upload.size)) {
    web_system_asset_upload_fail("只支持完整JPEG图片，最大400KB");
    return;
  }

  const char* final_path = web_system_asset_path(kind);
  if (!final_path ||
      !storage_config_stage_psram(
          final_path,
          s_system_asset_upload.data,
          s_system_asset_upload.size,
          web_validate_system_jpeg_file,
          nullptr,
          web_system_asset_commit_complete,
          nullptr)) {
    web_system_asset_upload_fail("图片暂存PSRAM失败，原图片未改变");
    return;
  }

  s_system_asset_upload.staged = true;
  const bool local_audio_open =
      player_source_type_get() == PlayerSourceType::LOCAL_TRACK &&
      (audio_service_is_playing() || audio_service_is_paused());
  if (local_audio_open) {
    s_system_asset_upload.deferred = true;
  } else {
    (void)storage_config_commit_pending(5000);
    s_system_asset_upload.deferred =
        storage_config_has_pending_path(final_path);
  }
}

static void web_handle_system_asset_upload_complete(WebSystemAssetKind kind)
{
  if (s_system_asset_upload.kind != kind ||
      !s_system_asset_upload.result_ready ||
      s_system_asset_upload.failed ||
      !s_system_asset_upload.staged) {
    const String message = s_system_asset_upload.error[0]
        ? String(s_system_asset_upload.error)
        : String("未收到有效JPEG图片");
    web_system_asset_upload_reset();
    web_send_json_err(message.c_str(), 400);
    return;
  }

  const char* final_path = web_system_asset_path(kind);
  String json = "{\"ok\":true,\"state\":\"";
  json += s_system_asset_upload.deferred ? "pending" : "committed";
  json += "\",\"path\":\"";
  json += web_json_escape(String(final_path ? final_path : ""));
  json += "\",\"size\":";
  json += String((unsigned)s_system_asset_upload.size);
  json += ",\"message\":\"";
  String message = web_system_asset_label(kind);
  message += s_system_asset_upload.deferred
      ? "已暂存PSRAM，下次切歌后写入TF卡"
      : "已写入TF卡，将在下次加载封面时生效";
  json += message;
  json += "\"}";

  web_system_asset_upload_reset();
  web_send_no_cache_headers();
  s_server.send(200, "application/json; charset=utf-8", json);
}

static void web_handle_default_cover_upload_stream()
{
  web_handle_system_asset_upload_stream(WebSystemAssetKind::DefaultCover);
}

static void web_handle_default_cover_upload_complete()
{
  web_handle_system_asset_upload_complete(WebSystemAssetKind::DefaultCover);
}

static void web_handle_net_cover_loading_upload_stream()
{
  web_handle_system_asset_upload_stream(WebSystemAssetKind::NetCoverLoading);
}

static void web_handle_net_cover_loading_upload_complete()
{
  web_handle_system_asset_upload_complete(WebSystemAssetKind::NetCoverLoading);
}

static void web_append_system_asset_status(String& json,
                                           const char* key,
                                           const char* path)
{
  bool exists = false;
  size_t size = 0;
  const bool pending = storage_config_has_pending_path(path);
  const bool local_audio_open =
      player_source_type_get() == PlayerSourceType::LOCAL_TRACK &&
      (audio_service_is_playing() || audio_service_is_paused());
  if (!pending && !local_audio_open && storage_is_ready()) {
    // 网页读取状态时顺便恢复掉电遗留的有效 .tmp/.bak；本地音频占卡时不执行。
    (void)storage_config_recover(path,
                                 web_validate_system_jpeg_file,
                                 nullptr,
                                 1500);
  }
  if (pending) {
    exists = true;
    size = storage_config_pending_size(path);
  } else if (storage_is_ready()) {
    StorageSdLockGuard guard(1000);
    if (guard && sd.exists(path)) {
      File32 file = sd.open(path, O_RDONLY);
      if (file) {
        exists = true;
        size = static_cast<size_t>(file.fileSize());
        file.close();
      }
    }
  }

  json += "\"";
  json += key;
  json += "\":{\"path\":\"";
  json += web_json_escape(String(path));
  json += "\",\"exists\":";
  json += exists ? "true" : "false";
  json += ",\"pending\":";
  json += pending ? "true" : "false";
  json += ",\"size\":";
  json += String((unsigned)size);
  json += "}";
}

static void web_handle_system_assets_get()
{
  String json;
  json.reserve(512);
  json = "{\"ok\":true,\"max_bytes\":";
  json += String((unsigned)kWebSystemAssetMaxBytes);
  json += ",";
  web_append_system_asset_status(json,
                                 "default_cover",
                                 SystemPaths::kDefaultCover);
  json += ",";
  web_append_system_asset_status(json,
                                 "net_cover_loading",
                                 SystemPaths::kNetCoverLoading);
  json += "}";
  web_send_no_cache_headers();
  s_server.send(200, "application/json; charset=utf-8", json);
}

static bool web_parse_int_arg(const char* name, int& out) {
  String s = s_server.arg(name);
  if (!s.length()) return false;
  out = s.toInt();
  return true;
}
static bool web_status_mode_is_artist() {
  return player_playlist_is_artist_mode(app_play_mode_get());
}
static bool web_status_mode_is_album() {
  return player_playlist_is_album_mode(app_play_mode_get());
}
static bool web_radio_catalog_ensure_loaded() {
  return radio_catalog_is_loaded();
}
static void web_send_radio_list_json() {
  const bool loaded = web_radio_catalog_ensure_loaded();
  const auto& items = radio_catalog_items();

  LOGD("[网页] 电台s 总计=%u (流-batch)", (unsigned)items.size());

  web_send_no_cache_headers();
  s_server.sendHeader("Connection", "close");
  s_server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  s_server.send(200, "application/json; charset=utf-8", "{");

  String head;
  head.reserve(256);
  head += "\"ok\":";
  head += (loaded ? "true" : "false");
  head += ",\"path\":\"";
  head += web_json_escape(radio_catalog_path());
  head += "\"";
  head += ",\"error\":\"";
  head += web_json_escape(radio_catalog_error());
  head += "\"";
  head += ",\"total\":";
  head += String((unsigned long)items.size());
  head += ",\"items\":[";
  if (!web_send_chunk(head)) return;

  String chunk;
  chunk.reserve(1024);

  for (size_t i = 0; i < items.size(); ++i) {
    const auto& it = items[i];

    if (i) chunk += ",";

    chunk += "{\"idx\":";
    chunk += String((unsigned long)i);

    chunk += ",\"name\":\"";
    chunk += web_json_escape(it.name);
    chunk += "\"";

    chunk += ",\"format\":\"";
    chunk += web_json_escape(it.format);
    chunk += "\"";

    chunk += ",\"region\":\"";
    chunk += web_json_escape(it.region);
    chunk += "\"";

    chunk += ",\"logo\":\"";
    chunk += web_json_escape(it.logo);
    chunk += "\"";

    chunk += ",\"url\":\"";
    chunk += web_json_escape(it.url);
    chunk += "\"}";

    if (chunk.length() >= 1024) {
      if (!web_flush_chunk_buffer(chunk)) return;
    }
  }

  if (!web_flush_chunk_buffer(chunk)) return;
  if (!web_send_chunk("]}")) return;
  web_end_stream_response();
}

static void web_send_group_list_json(const std::vector<PlaylistGroup>& groups, bool is_album) {
  const WebPlayerSnapshot snap = web_snapshot_capture();
  const MusicCatalogV3& cat = storage_catalog_v3();
  const int current_group_idx = player_playlist_get_current_group_idx();

  LOGD("[网页] 分组列表：类型=%s 总数=%u（流式批量输出）",
       is_album ? "album" : "artist",
       (unsigned)groups.size());

  web_send_no_cache_headers();
  s_server.sendHeader("Connection", "close");
  s_server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  s_server.send(200, "application/json; charset=utf-8", "{");

  String head;
  head.reserve(256);
  head += "\"ok\":true";
  head += ",\"total\":";
  head += String((unsigned long)groups.size());
  head += ",\"current_group_idx\":";
  head += String(current_group_idx);
  head += ",\"mode\":\"";
  head += web_json_escape(String(snap.mode));
  head += "\"";
  head += ",\"mode_label\":\"";
  head += web_json_escape(String(snap.mode_label));
  head += "\"";
  head += ",\"items\":[";
  if (!web_send_chunk(head)) return;

  String chunk;
  chunk.reserve(2048);

  for (size_t i = 0; i < groups.size(); ++i) {
    const auto& g = groups[i];
    const bool active = is_album
        ? (web_status_mode_is_album() && current_group_idx == (int)i)
        : (web_status_mode_is_artist() && current_group_idx == (int)i);

    String g_name = playlist_group_name_string(cat, g);
    String g_pa = playlist_group_primary_artist_string(cat, g);

    if (i) chunk += ",";

    chunk += "{\"idx\":";
    chunk += String((unsigned long)i);

    chunk += ",\"name\":\"";
    chunk += web_json_escape(g_name);
    chunk += "\"";

    if (is_album) {
      chunk += ",\"primary_artist\":\"";
      chunk += web_json_escape(g_pa);
      chunk += "\"";
    }

    chunk += ",\"track_count\":";
    chunk += String((unsigned long)g.track_indices.size());

    chunk += ",\"active\":";
    chunk += (active ? "true" : "false");

    chunk += "}";

    if (chunk.length() >= 1536) {
      if (!web_flush_chunk_buffer(chunk)) return;
    }
  }

  if (!web_flush_chunk_buffer(chunk)) return;
  if (!web_send_chunk("]}")) return;
  web_end_stream_response();
}
static void web_send_group_detail_json(const std::vector<PlaylistGroup>& groups, int group_idx, bool is_album) {
  if (group_idx < 0 || group_idx >= (int)groups.size()) {
    web_send_json_err("分组不存在", 404);
    return;
  }

  const auto& g = groups[(size_t)group_idx];
  const MusicCatalogV3& cat = storage_catalog_v3();
  String g_name = playlist_group_name_string(cat, g);
  String g_pa = playlist_group_primary_artist_string(cat, g);

  int offset = 0;
  int limit = 40;
  web_parse_int_arg("offset", offset);
  web_parse_int_arg("limit", limit);

  if (offset < 0) offset = 0;
  if (limit <= 0) limit = 40;
  if (limit > 80) limit = 80;

  String q = web_trim_copy(s_server.arg("q"));
  q.toLowerCase();

  std::vector<int> filtered_track_indices;
  filtered_track_indices.reserve(g.track_indices.size());

  for (size_t i = 0; i < g.track_indices.size(); ++i) {
    const int track_idx = (int)g.track_indices[i];
    if (!cat.tracks || track_idx < 0 || track_idx >= (int)cat.track_count) continue;

    const TrackRowV3& row = cat.tracks[(size_t)track_idx];
    const char* title_c = pool_str_v3(cat.pool, row.title_off);

    if (q.length() > 0) {
      if (!web_str_icontains(title_c, q)) continue;
    }

    filtered_track_indices.push_back(track_idx);
  }

  const int total_tracks = (int)filtered_track_indices.size();
  if (offset > total_tracks) offset = total_tracks;
  const int end = (offset + limit > total_tracks) ? total_tracks : (offset + limit);

  LOGD("[网页] 分组详情：索引=%d 是否专辑=%d 查询=%s 偏移=%d 限制=%d 返回数量=%d 总数=%d",
       group_idx,
       is_album ? 1 : 0,
       q.c_str(),
       offset,
       limit,
       end - offset,
       total_tracks);

  web_send_no_cache_headers();
  s_server.sendHeader("Connection", "close");
  s_server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  s_server.send(200, "application/json; charset=utf-8", "{");

  String head;
  head.reserve(320);
  head += "\"ok\":true";
  head += ",\"idx\":";
  head += String(group_idx);
  head += ",\"name\":\"";
  head += web_json_escape(g_name);
  head += "\"";

  if (is_album) {
    head += ",\"primary_artist\":\"";
    head += web_json_escape(g_pa);
    head += "\"";
  }

  head += ",\"track_count\":";
  head += String(total_tracks);
  head += ",\"filtered\":";
  head += (q.length() > 0 ? "true" : "false");
  head += ",\"query\":\"";
  head += web_json_escape(q);
  head += "\"";
  head += ",\"tracks\":[";

  if (!web_send_chunk(head)) return;

  String chunk;
  chunk.reserve(3072);
  bool first = true;

  for (int i = offset; i < end; ++i) {
    const int track_idx = filtered_track_indices[(size_t)i];
    if (!cat.tracks || track_idx < 0 || track_idx >= (int)cat.track_count) {
      continue;
    }

    const TrackRowV3& row = cat.tracks[(size_t)track_idx];
    const char* title_c  = pool_str_v3(cat.pool, row.title_off);
    const char* artist_c = pool_str_v3(cat.pool, row.artist_off);
    const char* album_c  = web_track_album_name_cstr(cat, row);

    if (!first) chunk += ",";
    first = false;

    chunk += "{\"track_idx\":";
    chunk += String(track_idx);

    chunk += ",\"title\":\"";
    web_json_append_escaped(chunk, title_c);
    chunk += "\"";

    if (is_album) {
      chunk += ",\"artist\":\"";
      web_json_append_escaped(chunk, artist_c);
      chunk += "\"";
    } else {
      chunk += ",\"album\":\"";
      web_json_append_escaped(chunk, album_c);
      chunk += "\"";
    }

    chunk += "}";

    if (chunk.length() >= 2560) {
      if (!web_flush_chunk_buffer(chunk)) return;
    }
  }

  if (!web_flush_chunk_buffer(chunk)) return;
  if (!web_send_chunk("]}")) return;
  web_end_stream_response();
}
static bool web_play_group_impl(bool is_album, int group_idx) {
  const auto& groups = is_album ? player_playlist_album_groups() : player_playlist_artist_groups();
  if (group_idx < 0 || group_idx >= (int)groups.size()) return false;

  const play_mode_t current_mode = app_play_mode_get();
  const bool keep_random = control_mode_is_random(current_mode);
  const play_mode_t next_mode = is_album
      ? (keep_random ? PLAY_MODE_ALBUM_RND : PLAY_MODE_ALBUM_SEQ)
      : (keep_random ? PLAY_MODE_ARTIST_RND : PLAY_MODE_ARTIST_SEQ);
  (void)app_play_mode_set(next_mode, AppPlayModeChangeReason::WebControl);

  player_playlist_set_current_group_idx(group_idx);
  player_playlist_force_rebuild();

  player_playlist_ensure_current();
  const int first_track = player_playlist_current_track_at(0);
  if (first_track < 0) return false;

  return player_play_idx_v3((uint32_t)first_track, true, true);
}
static void web_handle_artists_page() {
  web_send_no_cache_headers();
  s_server.send_P(200, "text/html; charset=utf-8", WEBCTRL_ARTISTS_HTML);
}
static void web_handle_nfc_page() {
  web_send_no_cache_headers();
  s_server.send_P(200, "text/html; charset=utf-8", WEBCTRL_NFC_HTML);
}
static void web_handle_albums_page() {
  web_send_no_cache_headers();
  s_server.send_P(200, "text/html; charset=utf-8", WEBCTRL_ALBUMS_HTML);
}
static void web_handle_root() {
  web_send_no_cache_headers();
  s_server.send_P(200, "text/html; charset=utf-8", WEBCTRL_INDEX_HTML);
}
static void web_handle_settings_page() {
  web_send_no_cache_headers();
  s_server.send_P(200, "text/html; charset=utf-8", WEBCTRL_SETTINGS_HTML);
}
static void web_handle_feedback_js() {
  web_send_no_cache_headers();
  s_server.send_P(200, "application/javascript; charset=utf-8", WEBCTRL_FEEDBACK_JS);
}
static void web_handle_favicon() { web_send_no_cache_headers(); s_server.send(404, "text/plain; charset=utf-8", "not_found"); }
static void web_handle_wifi_config_get()
{
  std::vector<WebWifiNetwork> networks;
  String hostname;
  String error;
  bool loaded_from_pending = false;
  const bool configured = web_load_wifi_config(networks,
                                               hostname,
                                               &error,
                                               &loaded_from_pending);
  if (!configured) {
    networks.clear();
    hostname = WEBCTRL_HOSTNAME_DEFAULT;
  }

  const bool connected = WiFi.status() == WL_CONNECTED;
  const String active_ssid = connected ? WiFi.SSID() : String("");

  String json;
  json.reserve(640 + networks.size() * 190);
  json += "{\"ok\":true";
  json += ",\"path\":\"";
  json += web_json_escape(SystemPaths::kWifiConfig);
  json += "\"";
  json += ",\"configured\":";
  json += configured ? "true" : "false";
  json += ",\"error\":\"";
  json += web_json_escape(error);
  json += "\"";
  json += ",\"hostname\":\"";
  json += web_json_escape(hostname);
  json += "\"";
  json += ",\"wifi_enabled\":";
  json += s_wifi_enabled ? "true" : "false";
  json += ",\"connected\":";
  json += connected ? "true" : "false";
  json += ",\"mode\":\"";
  json += web_net_mode_cstr();
  json += "\"";
  json += ",\"ip\":\"";
  json += web_json_escape(web_ip_string());
  json += "\"";
  json += ",\"active_ssid\":\"";
  json += web_json_escape(active_ssid);
  json += "\"";
  json += ",\"write_pending\":";
  json += loaded_from_pending ? "true" : "false";
  json += ",\"apply_pending\":";
  json += web_wifi_config_apply_is_pending() ? "true" : "false";
  json += ",\"max_networks\":";
  json += String((unsigned)kWebWifiMaxNetworks);
  json += ",\"networks\":[";

  for (size_t i = 0; i < networks.size(); ++i) {
    if (i) json += ',';
    const WebWifiNetwork& network = networks[i];
    json += "{\"index\":";
    json += String((unsigned)i);
    json += ",\"ssid\":\"";
    json += web_json_escape(network.ssid);
    json += "\"";
    json += ",\"has_password\":";
    json += network.password.length() ? "true" : "false";
    json += ",\"hidden\":";
    json += network.hidden ? "true" : "false";
    json += ",\"channel\":";
    json += String(network.channel);
    json += ",\"bssid\":\"";
    json += web_json_escape(web_wifi_format_bssid(network));
    json += "\"";
    json += ",\"active\":";
    json += (connected && network.ssid == active_ssid) ? "true" : "false";
    json += '}';
  }
  json += "]}";

  web_send_no_cache_headers();
  s_server.send(200, "application/json; charset=utf-8", json);
}

static void web_handle_wifi_config_save_common(bool apply_after_save)
{
  if (!storage_is_ready()) {
    web_send_json_err("TF卡未就绪，无法保存WiFi配置", 503);
    return;
  }

  String error;
  bool deferred = false;
  if (!web_stage_wifi_config_from_request(apply_after_save, error, deferred)) {
    web_send_json_err(error.c_str(), 400);
    return;
  }

  const char* state = deferred ? "pending" : "committed";
  const char* message = nullptr;
  if (deferred) {
    message = apply_after_save
        ? "WiFi配置已暂存PSRAM，下次切歌后写入TF卡并重连"
        : "WiFi配置已暂存PSRAM，下次切歌后写入TF卡";
  } else {
    message = apply_after_save
        ? "WiFi配置已写入TF卡，设备即将按新顺序重连"
        : "WiFi配置已写入TF卡，当前连接保持不变";
  }

  String json = "{\"ok\":true,\"state\":\"";
  json += state;
  json += "\",\"message\":\"";
  json += web_json_escape(String(message));
  json += "\"}";
  web_send_no_cache_headers();
  s_server.send(200, "application/json; charset=utf-8", json);
}

static void web_handle_wifi_config_save()
{
  web_handle_wifi_config_save_common(false);
}

static void web_handle_wifi_config_apply()
{
  web_handle_wifi_config_save_common(true);
}

static void web_handle_settings_get() {
  const WebRuntimeSettings ws = web_settings_get();
  String json; json.reserve(720);
  json += "{\"ok\":true";
  json += ",\"refresh_preset\":\"" + String(web_refresh_preset_key(ws.refresh_preset)) + "\"";
  json += ",\"refresh_preset_label\":\"" + String(web_refresh_preset_label(ws.refresh_preset)) + "\"";
  json += ",\"refresh_poll_ms\":" + String((unsigned long)web_refresh_preset_poll_ms(ws.refresh_preset));
  json += ",\"lyric_sync_mode\":\"" + String(web_lyric_sync_mode_key(ws.lyric_sync_mode)) + "\"";
  json += ",\"lyric_sync_mode_label\":\"" + String(web_lyric_sync_mode_label(ws.lyric_sync_mode)) + "\"";
  json += ",\"lyric_wait_poll_threshold_ms\":" + String((unsigned long)web_lyric_sync_mode_threshold_ms(ws.lyric_sync_mode));
  json += ",\"show_next_lyric\":"; json += (ws.show_next_lyric ? "true" : "false");
  json += ",\"show_cover\":"; json += (ws.show_cover ? "true" : "false");
  json += ",\"web_cover_spin\":"; json += (ws.web_cover_spin ? "true" : "false");
  json += ",\"hall_control_enabled\":"; json += (ws.hall_control_enabled ? "true" : "false");
  json += ",\"solenoid_enabled\":"; json += (ws.solenoid_enabled ? "true" : "false");
  json += ",\"solenoid_direction_inverted\":";
  json += (ws.solenoid_direction_inverted ? "true" : "false");
  json += ",\"status_led_enabled\":"; json += (ws.status_led_enabled ? "true" : "false");
  json += ",\"status_led_brightness\":\"" + String(status_led_brightness_key(ws.status_led_brightness)) + "\"";
  json += ",\"device_view\":\"" + String(web_device_view_key(ui_get_view())) + "\"";
  json += ",\"screen_enabled\":"; json += (board_hw_get_backlight() ? "true" : "false");
  json += ",\"sleep_timer_minutes\":" + String((unsigned int)app_power_sleep_timer_preset_minutes());
  json += ",\"sleep_timer_remaining_seconds\":" + String((unsigned long)app_power_sleep_timer_remaining_seconds());
  json += "}";
  web_send_no_cache_headers();
  s_server.send(200, "application/json; charset=utf-8", json);
}
static void web_handle_settings_post() {
  const WebRuntimeSettings old_ws = web_settings_get();
  WebRuntimeSettings ws = old_ws;

  String refresh = s_server.arg("refresh_preset");
  if (refresh.length()) {
    String s = refresh; s.toLowerCase();
    if (s == "power" || s == "power_save") ws.refresh_preset = WebRefreshPreset::POWER_SAVE;
    else if (s == "smooth") ws.refresh_preset = WebRefreshPreset::SMOOTH;
    else ws.refresh_preset = WebRefreshPreset::BALANCED;
  }
  String lyric = s_server.arg("lyric_sync_mode");
  if (lyric.length()) {
    String s = lyric; s.toLowerCase();
    if (s == "precise") ws.lyric_sync_mode = WebLyricSyncMode::PRECISE;
    else if (s == "follow_poll" || s == "wait_poll") ws.lyric_sync_mode = WebLyricSyncMode::FOLLOW_POLL;
    else ws.lyric_sync_mode = WebLyricSyncMode::BALANCED;
  }
  ws.show_next_lyric = web_parse_bool(s_server.arg("show_next_lyric"), ws.show_next_lyric);
  ws.show_cover = web_parse_bool(s_server.arg("show_cover"), ws.show_cover);
  ws.web_cover_spin = web_parse_bool(s_server.arg("web_cover_spin"), ws.web_cover_spin);
  ws.hall_control_enabled = web_parse_bool(s_server.arg("hall_control_enabled"), ws.hall_control_enabled);
  ws.solenoid_enabled = web_parse_bool(s_server.arg("solenoid_enabled"), ws.solenoid_enabled);
  ws.solenoid_direction_inverted = web_parse_bool(
      s_server.arg("solenoid_direction_inverted"),
      ws.solenoid_direction_inverted);
  if (ws.solenoid_enabled) {
    // 电磁铁开启时霍尔必须参与到位确认，忽略网页提交的关闭值。
    ws.hall_control_enabled = true;
  }
  ws.status_led_enabled = web_parse_bool(s_server.arg("status_led_enabled"), ws.status_led_enabled);

  const String led_brightness_arg = web_trim_copy(s_server.arg("status_led_brightness"));
  if (led_brightness_arg.length()) {
    String key = led_brightness_arg;
    key.toLowerCase();
    if (key == "low") ws.status_led_brightness = StatusLedBrightness::Low;
    else if (key == "medium") ws.status_led_brightness = StatusLedBrightness::Medium;
    else if (key == "high") ws.status_led_brightness = StatusLedBrightness::High;
    else { web_send_json_err("状态灯亮度参数无效", 400); return; }
  }

  ui_player_view_t requested_view = ui_get_view();
  const bool has_device_view = s_server.hasArg("device_view") && s_server.arg("device_view").length();
  if (has_device_view && !web_parse_device_view(s_server.arg("device_view"), &requested_view)) {
    web_send_json_err("设备显示类型无效", 400);
    return;
  }

  const bool has_screen_enabled = s_server.hasArg("screen_enabled");
  const bool requested_screen_enabled = has_screen_enabled
      ? web_parse_bool(s_server.arg("screen_enabled"), board_hw_get_backlight())
      : board_hw_get_backlight();

  uint16_t sleep_timer_minutes = app_power_sleep_timer_preset_minutes();
  const bool has_sleep_timer = s_server.hasArg("sleep_timer_minutes")
      && s_server.arg("sleep_timer_minutes").length();
  if (has_sleep_timer) {
    const String value = web_trim_copy(s_server.arg("sleep_timer_minutes"));
    if (value != "0" && value != "15" && value != "30" && value != "60" && value != "90") {
      web_send_json_err("睡眠关机档位无效", 400);
      return;
    }
    sleep_timer_minutes = static_cast<uint16_t>(value.toInt());
    if (!web_sleep_timer_minutes_valid(sleep_timer_minutes)) {
      web_send_json_err("睡眠关机档位无效", 400);
      return;
    }
  }

  // 屏幕控制先执行；硬件失败时不提交其它设置，避免网页显示保存成功但屏幕未切换。
  if (has_screen_enabled && requested_screen_enabled != board_hw_get_backlight()) {
    if (!board_hw_set_backlight(requested_screen_enabled)) {
      web_send_json_err("屏幕开关失败", 500);
      return;
    }
  }

  if (has_device_view && requested_view != ui_get_view()) {
    ui_set_view(requested_view);
  }
  if (has_sleep_timer) {
    app_power_sleep_timer_set_minutes(sleep_timer_minutes);
  }

  const bool force_persist = web_parse_bool(s_server.arg("persist"), false);
  const bool need_immediate_save = force_persist || web_settings_persistent_core_changed(old_ws, ws);
  web_settings_set(ws);

  if (old_ws.solenoid_enabled && !ws.solenoid_enabled) {
    (void)board_hw_solenoid_stop();
  }
  if (old_ws.status_led_enabled != ws.status_led_enabled) {
    if (ws.status_led_enabled) ws2812_status_force_refresh();
    else ws2812_status_off();
  } else if (old_ws.status_led_brightness != ws.status_led_brightness && ws.status_led_enabled) {
    ws2812_status_force_refresh();
  }

  if (need_immediate_save) {
    if (!web_settings_save_if_dirty()) { web_send_json_err("保存设置失败", 500); return; }
    web_send_json_ok_simple("settings_saved");
    return;
  }

  // 网页显示开关立即生效，关机前统一写入 NVS。
  web_send_json_ok_simple(web_settings_is_dirty() ? "settings_deferred" : "settings_unchanged");
}

static const char* web_audio_output_route_key(AudioOutputRoute route)
{
  switch (route) {
    case AudioOutputRoute::HeadphoneOnly: return "headphone";
    case AudioOutputRoute::BluetoothTx:   return "bluetooth";
    case AudioOutputRoute::Speaker:
    default:                              return "speaker";
  }
}

static const char* web_bt_device_state_key(Bt62spConnectedDeviceState state)
{
  switch (state) {
    case Bt62spConnectedDeviceState::Querying:            return "querying";
    case Bt62spConnectedDeviceState::Connected:           return "connected";
    case Bt62spConnectedDeviceState::ConnectedNoIdentity: return "connected_no_identity";
    case Bt62spConnectedDeviceState::NotConnected:        return "not_connected";
    case Bt62spConnectedDeviceState::Timeout:             return "timeout";
    case Bt62spConnectedDeviceState::ParseError:          return "parse_error";
    case Bt62spConnectedDeviceState::Unknown:
    default:                                               return "unknown";
  }
}

static void web_send_audio_output_status_json()
{
  const AudioOutputRouteSnapshot route = audio_output_route_snapshot_get();
  const BluetoothRestartSnapshot restart = bluetooth_restart_snapshot_get();
  const bool bt_power = board_hw_get_bt_power();

  bool amp_muted = board_hw_get_amp_mute();
  bool amp_mute_known = false;
  bool amp_shutdown = board_hw_get_amp_shutdown();
  bool amp_shutdown_known = false;
  if (route.route == AudioOutputRoute::Speaker) {
    amp_mute_known = board_hw_read_amp_mute(&amp_muted);
    amp_shutdown_known = board_hw_read_amp_shutdown(&amp_shutdown);
  }

  bool bt_linked = false;
  const bool bt_link_known = bt_power && board_hw_read_bt_link(&bt_linked);
  const Bt62spConnectedDeviceSnapshot device =
      bt62sp_uart_debug_connected_device_snapshot_get();

  String json;
  json.reserve(768);
  json += "{\"ok\":true";
  json += ",\"route\":\"";
  json += web_audio_output_route_key(route.route);
  json += "\"";
  json += ",\"route_label\":\"";
  json += web_json_escape(String(audio_output_route_label()));
  json += "\"";
  json += ",\"route_revision\":";
  json += String((unsigned long)route.revision);
  json += ",\"user_volume\":";
  json += String((unsigned)audio_output_route_get_user_volume());

  json += ",\"amp_mute_known\":";
  json += amp_mute_known ? "true" : "false";
  json += ",\"amp_muted\":";
  json += amp_muted ? "true" : "false";
  json += ",\"amp_shutdown_known\":";
  json += amp_shutdown_known ? "true" : "false";
  json += ",\"amp_shutdown\":";
  json += amp_shutdown ? "true" : "false";

  json += ",\"bt_power\":";
  json += bt_power ? "true" : "false";
  json += ",\"bt_link_known\":";
  json += bt_link_known ? "true" : "false";
  json += ",\"bt_linked\":";
  json += bt_linked ? "true" : "false";
  json += ",\"bt_device_state\":\"";
  json += web_bt_device_state_key(device.state);
  json += "\"";
  json += ",\"bt_device_name\":\"";
  json += web_json_escape(String(device.name));
  json += "\"";
  json += ",\"bt_device_mac\":\"";
  json += web_json_escape(String(device.mac));
  json += "\"";
  json += ",\"bt_device_revision\":";
  json += String((unsigned long)device.revision);

  json += ",\"bt_restart_in_progress\":";
  json += restart.in_progress ? "true" : "false";
  json += ",\"bt_restart_result\":";
  json += String((unsigned)restart.last_result);
  json += ",\"can_amp_control\":";
  json += (route.route == AudioOutputRoute::Speaker && !restart.in_progress) ? "true" : "false";
  json += ",\"can_bt_control\":";
  json += (route.route == AudioOutputRoute::BluetoothTx && bt_power && !restart.in_progress)
      ? "true" : "false";
  json += ",\"can_bt_query\":";
  json += (route.route == AudioOutputRoute::BluetoothTx && bt_power && bt_link_known
           && bt_linked && !restart.in_progress)
      ? "true" : "false";
  json += "}";

  web_send_no_cache_headers();
  s_server.send(200, "application/json; charset=utf-8", json);
}

static void web_handle_audio_output_status()
{
  web_send_audio_output_status_json();
}

static void web_handle_audio_output_route()
{
  if (bluetooth_restart_is_in_progress()) {
    web_send_json_err("蓝牙正在重启，暂不能切换输出路径", 409);
    return;
  }

  String key = web_trim_copy(s_server.arg("route"));
  key.toLowerCase();

  bool accepted = false;
  if (key == "headphone") {
    accepted = audio_output_route_select_headphone_only();
  } else if (key == "speaker") {
    accepted = audio_output_route_select_speaker();
  } else if (key == "bluetooth") {
    accepted = audio_output_route_select_bluetooth_tx();
  } else {
    web_send_json_err("音频输出路径无效", 400);
    return;
  }

  if (!accepted) {
    web_send_json_err("音频任务繁忙，输出路径切换未入队", 503);
    return;
  }
  web_send_json_ok_simple("route_switch_queued");
}

static void web_handle_audio_output_amp_mute()
{
  if (!audio_output_route_is_speaker()) {
    web_send_json_err("当前不是功放输出路径", 409);
    return;
  }
  const bool enabled = web_parse_bool(s_server.arg("enabled"), board_hw_get_amp_mute());
  if (!audio_output_route_set_amp_mute(enabled)) {
    web_send_json_err("音频任务繁忙，功放静音请求未入队", 503);
    return;
  }
  web_send_json_ok_simple("amp_mute_queued");
}

static bool web_require_bluetooth_output(bool require_link = false)
{
  if (!audio_output_route_is_bluetooth_tx()) {
    web_send_json_err("当前不是蓝牙输出路径", 409);
    return false;
  }
  if (bluetooth_restart_is_in_progress()) {
    web_send_json_err("蓝牙模块正在重启", 409);
    return false;
  }
  if (!board_hw_get_bt_power()) {
    web_send_json_err("蓝牙模块未上电", 409);
    return false;
  }
  if (require_link) {
    bool linked = false;
    if (!board_hw_read_bt_link(&linked)) {
      web_send_json_err("蓝牙连接状态读取失败", 500);
      return false;
    }
    if (!linked) {
      web_send_json_err("当前没有已连接蓝牙设备", 409);
      return false;
    }
  }
  return true;
}

static void web_handle_audio_output_bt_query()
{
  if (!web_require_bluetooth_output(true)) return;
  if (!bt62sp_uart_debug_request_connected_device_query()) {
    web_send_json_err("蓝牙命令繁忙，设备查询未入队", 503);
    return;
  }
  web_send_json_ok_simple("device_query_queued");
}

static void web_handle_audio_output_bt_volume()
{
  if (!web_require_bluetooth_output(false)) return;
  String value = web_trim_copy(s_server.arg("value"));
  if (!value.length()) {
    web_send_json_err("缺少蓝牙音量参数 value", 400);
    return;
  }
  const int volume = value.toInt();
  if (volume < 0 || volume > 100) {
    web_send_json_err("蓝牙音量必须在0到100之间", 400);
    return;
  }
  if (!audio_output_route_set_user_volume(static_cast<uint8_t>(volume))) {
    web_send_json_err("音频任务繁忙，蓝牙音量请求未入队", 503);
    return;
  }
  ui_volume_key_pressed();
  web_send_json_ok_simple("bluetooth_volume_queued");
}

static void web_handle_audio_output_bt_pair()
{
  if (!web_require_bluetooth_output(false)) return;
  if (!board_hw_pulse_bt_switch(200)) {
    web_send_json_err("蓝牙配对按键操作失败", 500);
    return;
  }
  web_send_json_ok_simple("pair_pulse_sent");
}

static void web_handle_audio_output_bt_restart()
{
  if (!web_require_bluetooth_output(false)) return;
  if (!bluetooth_restart_request(BluetoothRestartPolicy::RequireBluetoothTxRoute)) {
    web_send_json_err("蓝牙重启请求失败", 503);
    return;
  }
  web_send_json_ok_simple("bluetooth_restart_started");
}

static uint32_t web_task_stack_free_bytes(TaskHandle_t handle)
{
  if (!handle) return 0;
  return static_cast<uint32_t>(uxTaskGetStackHighWaterMark(handle));
}

static const char* web_battery_power_state_key(const BatteryUiStatus& battery)
{
  if (!battery.valid) return "unavailable";
  if (battery.charging) return "charging";
  if (battery.external_power_good) return "external_power";
  return "discharging";
}

static void web_append_battery_diagnostics_json(String& json,
                                                const BatteryUiStatus& battery,
                                                uint32_t now_ms)
{
  const uint32_t age_ms = battery.updated_ms != 0
      ? static_cast<uint32_t>(now_ms - battery.updated_ms)
      : 0;
  const bool runtime_ready =
      battery.runtime_estimate_state == BatteryRuntimeEstimateState::Ready ||
      battery.runtime_estimate_state == BatteryRuntimeEstimateState::BluetoothEstimated;

  json += "\"battery\":{";
  json += "\"valid\":";
  json += battery.valid ? "true" : "false";
  json += ",\"percent\":" + String(static_cast<unsigned>(battery.percent));
  json += ",\"voltage_mv\":" + String(static_cast<unsigned long>(battery.mv_battery));
  json += ",\"current_ma\":" + String(static_cast<int>(battery.average_current_ma));
  json += ",\"remaining_capacity_mah\":" + String(static_cast<unsigned>(battery.remaining_capacity_mah));
  json += ",\"full_charge_capacity_mah\":" + String(static_cast<unsigned>(battery.full_charge_capacity_mah));
  json += ",\"design_capacity_mah\":" + String(static_cast<unsigned>(battery.design_capacity_mah));
  json += ",\"state_of_health_percent\":" + String(static_cast<unsigned>(battery.state_of_health_percent));
  json += ",\"charging\":";
  json += battery.charging ? "true" : "false";
  json += ",\"external_power_good\":";
  json += battery.external_power_good ? "true" : "false";
  json += ",\"power_state\":\"";
  json += web_battery_power_state_key(battery);
  json += "\"";
  json += ",\"runtime_ready\":";
  json += runtime_ready ? "true" : "false";
  json += ",\"runtime_minutes\":" + String(static_cast<unsigned long>(battery.estimated_runtime_minutes));
  json += ",\"runtime_state\":" + String(static_cast<unsigned>(battery.runtime_estimate_state));
  json += ",\"runtime_label\":\"";
  json += web_json_escape(String(board_hw_battery_runtime_state_label(battery.runtime_estimate_state)));
  json += "\"";
  json += ",\"estimated_discharge_current_ma\":" + String(static_cast<unsigned>(battery.estimated_discharge_current_ma));
  json += ",\"sample_age_ms\":" + String(static_cast<unsigned long>(age_ms));
  json += ",\"bq_ready\":";
  json += bq27441_is_ready() ? "true" : "false";
  json += ",\"bq_i2c_error\":" + String(static_cast<unsigned>(bq27441_last_i2c_error()));
  json += "}";
}

static void web_handle_system_diagnostics()
{
  const uint32_t now_ms = millis();
  const BatteryUiStatus battery = board_hw_get_battery_status_cached();
  String scope = web_trim_copy(s_server.arg("scope"));
  scope.toLowerCase();
  const bool battery_only = scope == "battery";

  String json;
  json.reserve(battery_only ? 760 : 2600);
  json += "{\"ok\":true,\"sample_ms\":";
  json += String(static_cast<unsigned long>(now_ms));
  json += ",";
  web_append_battery_diagnostics_json(json, battery, now_ms);

  if (!battery_only) {
    const uint32_t internal_free =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t internal_largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t dma_free =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    const uint32_t dma_largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    const bool psram_ready = psramFound();
    const bool wifi_connected = s_ap_mode || WiFi.status() == WL_CONNECTED;
    const int32_t wifi_rssi = (!s_ap_mode && WiFi.status() == WL_CONNECTED)
        ? WiFi.RSSI()
        : 0;

    json += ",\"system\":{";
    json += "\"firmware\":\"2.5.0\"";
    json += ",\"uptime_ms\":" + String(static_cast<unsigned long>(now_ms));
    json += ",\"app_state\":" + String(static_cast<unsigned>(g_app_state));
    json += "}";

    json += ",\"memory\":{";
    json += "\"heap_free\":" + String(static_cast<unsigned long>(ESP.getFreeHeap()));
    json += ",\"heap_min_free\":" + String(static_cast<unsigned long>(ESP.getMinFreeHeap()));
    json += ",\"internal_free\":" + String(static_cast<unsigned long>(internal_free));
    json += ",\"internal_largest\":" + String(static_cast<unsigned long>(internal_largest));
    json += ",\"dma_free\":" + String(static_cast<unsigned long>(dma_free));
    json += ",\"dma_largest\":" + String(static_cast<unsigned long>(dma_largest));
    json += ",\"psram_ready\":";
    json += psram_ready ? "true" : "false";
    json += ",\"psram_free\":" + String(static_cast<unsigned long>(psram_ready ? ESP.getFreePsram() : 0));
    json += ",\"psram_total\":" + String(static_cast<unsigned long>(psram_ready ? ESP.getPsramSize() : 0));
    json += "}";

    json += ",\"tasks\":{";
    json += "\"audio\":" + String(static_cast<unsigned long>(web_task_stack_free_bytes(audio_service_get_task_handle())));
    json += ",\"ui\":" + String(static_cast<unsigned long>(web_task_stack_free_bytes(ui_get_task_handle())));
    json += ",\"loop\":" + String(static_cast<unsigned long>(web_task_stack_free_bytes(xTaskGetHandle("loopTask"))));
    json += ",\"asset\":" + String(static_cast<unsigned long>(web_task_stack_free_bytes(xTaskGetHandle("PlayerAssetTask"))));
    json += ",\"net_cover\":" + String(static_cast<unsigned long>(web_task_stack_free_bytes(xTaskGetHandle("NetCoverTask"))));
    json += ",\"flac_prefetch\":" + String(static_cast<unsigned long>(web_task_stack_free_bytes(xTaskGetHandle("FlacNetTask"))));
    json += ",\"rescan\":" + String(static_cast<unsigned long>(web_task_stack_free_bytes(xTaskGetHandle("rescan_v3"))));
    json += "}";

    json += ",\"hardware\":{";
    json += "\"i2c_ready\":";
    json += i2c_bus_is_ready() ? "true" : "false";
    json += ",\"i2c_clock_hz\":" + String(static_cast<unsigned long>(i2c_bus_clock_hz()));
    json += ",\"i2c_generation\":" + String(static_cast<unsigned long>(i2c_bus_generation()));
    json += ",\"mcp23017_ready\":";
    json += mcp23017_u3_is_ready() ? "true" : "false";
    json += ",\"bq27441_ready\":";
    json += bq27441_is_ready() ? "true" : "false";
    json += ",\"rtc_ready\":";
    json += pcf85063_is_ready() ? "true" : "false";
    json += ",\"rtc_status\":\"";
    json += web_json_escape(String(pcf85063_is_ready() ? pcf85063_status_label() : "ERR"));
    json += "\"";
    json += "}";

    json += ",\"network\":{";
    json += "\"connected\":";
    json += wifi_connected ? "true" : "false";
    json += ",\"mode\":\"";
    json += web_net_mode_cstr();
    json += "\"";
    json += ",\"ip\":\"";
    json += web_json_escape(web_ip_string());
    json += "\"";
    json += ",\"rssi_dbm\":" + String(static_cast<long>(wifi_rssi));
    json += "}";

    const AudioOutputRoute route = audio_output_route_get();
    json += ",\"audio\":{";
    json += "\"route\":\"";
    json += web_audio_output_route_key(route);
    json += "\"";
    json += ",\"volume\":" + String(static_cast<unsigned>(audio_output_route_get_user_volume()));
    json += ",\"playing\":";
    json += audio_service_is_playing() ? "true" : "false";
    json += ",\"paused\":";
    json += audio_service_is_paused() ? "true" : "false";
    json += "}";
  }

  json += "}";
  web_send_no_cache_headers();
  s_server.send(200, "application/json; charset=utf-8", json);
}

static void web_append_rtc_json_fields(String& json, const Pcf85063Status& st) {
  json += "\"ok\":true";
  json += ",\"ready\":";
  json += (st.ready ? "true" : "false");
  json += ",\"time_valid\":";
  json += (st.time_valid ? "true" : "false");
  json += ",\"oscillator_stopped\":";
  json += (st.oscillator_stopped ? "true" : "false");
  json += ",\"alarm_pending\":";
  json += (st.alarm_pending ? "true" : "false");
  json += ",\"alarm_enabled\":";
  json += (st.alarm_enabled ? "true" : "false");
  json += ",\"timer_pending\":";
  json += (st.timer_pending ? "true" : "false");
  json += ",\"boot_alarm\":";
  json += (pcf85063_boot_alarm_was_pending() ? "true" : "false");
  json += ",\"rtc_int_known\":";
  json += (st.rtc_int_level_known ? "true" : "false");
  json += ",\"rtc_int_level\":";
  json += (st.rtc_int_level ? "true" : "false");
  json += ",\"control2\":";
  json += String(st.control2);

  char hexbuf[8];
  snprintf(hexbuf, sizeof(hexbuf), "0x%02X", st.control2);
  json += ",\"control2_hex\":\"";
  json += hexbuf;
  json += "\"";

  json += ",\"status_label\":\"";
  json += web_json_escape(String(pcf85063_status_label()));
  json += "\"";

  json += ",\"alarm_label\":\"";
  json += web_json_escape(String(pcf85063_alarm_status_label()));
  json += "\"";

  json += ",\"datetime\":\"";
  json += web_json_escape(String(st.time_valid ? pcf85063_datetime_to_text(st.time) : "未设置"));
  json += "\"";

  json += ",\"year\":";
  json += String((unsigned)st.time.year);
  json += ",\"month\":";
  json += String((unsigned)st.time.month);
  json += ",\"day\":";
  json += String((unsigned)st.time.day);
  json += ",\"weekday\":";
  json += String((unsigned)st.time.weekday);
  json += ",\"hour\":";
  json += String((unsigned)st.time.hour);
  json += ",\"minute\":";
  json += String((unsigned)st.time.minute);
  json += ",\"second\":";
  json += String((unsigned)st.time.second);
}

static void web_send_rtc_status_json() {
  Pcf85063Status st{};
  const bool ok = pcf85063_read_status(&st);
  if (!ok) {
    web_send_json_err("RTC状态读取失败", 500);
    return;
  }

  String json;
  json.reserve(512);
  json += "{";
  web_append_rtc_json_fields(json, st);
  json += "}";
  web_send_no_cache_headers();
  s_server.send(200, "application/json; charset=utf-8", json);
}

static void web_handle_rtc_status() {
  web_send_rtc_status_json();
}

static bool web_parse_rtc_int_arg(const char* name, int& out) {
  if (!s_server.hasArg(name)) return false;
  String s = s_server.arg(name);
  s.trim();
  if (!s.length()) return false;
  out = s.toInt();
  return true;
}

static bool web_validate_rtc_fields(const Pcf85063DateTime& t) {
  return t.year >= 2000 && t.year <= 2099 &&
         t.month >= 1 && t.month <= 12 &&
         t.day >= 1 && t.day <= 31 &&
         t.weekday <= 6 &&
         t.hour <= 23 &&
         t.minute <= 59 &&
         t.second <= 59;
}

static void web_handle_rtc_set_time() {
  int year = 0;
  int month = 0;
  int day = 0;
  int weekday = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;

  if (!web_parse_rtc_int_arg("year", year) ||
      !web_parse_rtc_int_arg("month", month) ||
      !web_parse_rtc_int_arg("day", day) ||
      !web_parse_rtc_int_arg("weekday", weekday) ||
      !web_parse_rtc_int_arg("hour", hour) ||
      !web_parse_rtc_int_arg("minute", minute) ||
      !web_parse_rtc_int_arg("second", second)) {
    web_send_json_err("缺少RTC时间参数");
    return;
  }

  Pcf85063DateTime t{};
  t.year = (uint16_t)year;
  t.month = (uint8_t)month;
  t.day = (uint8_t)day;
  t.weekday = (uint8_t)weekday;
  t.hour = (uint8_t)hour;
  t.minute = (uint8_t)minute;
  t.second = (uint8_t)second;
  t.valid = true;
  t.oscillator_stopped = false;

  if (!web_validate_rtc_fields(t)) {
    web_send_json_err("RTC时间参数无效");
    return;
  }

  if (!pcf85063_set_time(t)) {
    web_send_json_err("RTC时间写入失败", 500);
    return;
  }

  (void)pcf85063_clear_interrupt_flags();
  web_send_rtc_status_json();
}


static void web_append_alarm_json_fields(String& json)
{
  const AppAlarmConfig cfg = app_alarm_get_config();
  char next_text[64];
  char weekday_text[64];
  const bool has_next = app_alarm_next_trigger_text(next_text, sizeof(next_text));
  if (cfg.repeat_mode == AppAlarmRepeatMode::ONCE) {
    snprintf(weekday_text, sizeof(weekday_text), "下一次");
  } else {
    (void)app_alarm_weekday_mask_to_text(app_alarm_effective_weekday_mask(cfg), weekday_text, sizeof(weekday_text));
  }

  json += "\"ok\":true";
  json += ",\"enabled\":";
  json += (cfg.enabled ? "true" : "false");
  json += ",\"hour\":";
  json += String((unsigned)cfg.hour);
  json += ",\"minute\":";
  json += String((unsigned)cfg.minute);
  json += ",\"second\":";
  json += String((unsigned)cfg.second);
  json += ",\"repeat\":\"";
  json += web_json_escape(String(app_alarm_repeat_key(cfg.repeat_mode)));
  json += "\"";
  json += ",\"repeat_label\":\"";
  json += web_json_escape(String(app_alarm_repeat_label(cfg.repeat_mode)));
  json += "\"";
  json += ",\"weekday_mask\":";
  json += String((unsigned)(cfg.weekday_mask & APP_ALARM_WEEKDAY_ALL));
  json += ",\"weekday_text\":\"";
  json += web_json_escape(String(weekday_text));
  json += "\"";
  json += ",\"action\":\"";
  json += web_json_escape(String(app_alarm_action_key(cfg.action)));
  json += "\"";
  json += ",\"action_label\":\"";
  json += web_json_escape(String(app_alarm_action_label(cfg.action)));
  json += "\"";
  json += ",\"volume\":";
  json += String((unsigned)cfg.volume);
  json += ",\"next_text\":\"";
  json += web_json_escape(String(next_text));
  json += "\"";
  json += ",\"has_next\":";
  json += (has_next ? "true" : "false");
  json += ",\"schedule_ok\":";
  json += (app_alarm_last_schedule_ok() ? "true" : "false");
  json += ",\"schedule_message\":\"";
  json += web_json_escape(String(app_alarm_last_schedule_message()));
  json += "\"";
}

static void web_send_alarm_status_json()
{
  String json;
  json.reserve(640);
  json += "{";
  web_append_alarm_json_fields(json);
  json += "}";
  web_send_no_cache_headers();
  s_server.send(200, "application/json; charset=utf-8", json);
}

static void web_handle_alarm_status()
{
  web_send_alarm_status_json();
}

static bool web_parse_alarm_config_from_args(AppAlarmConfig& cfg)
{
  int hour = 0;
  int minute = 0;
  int second = 0;
  int volume = 0;
  int weekday_mask = 0;
  if (!web_parse_rtc_int_arg("hour", hour) ||
      !web_parse_rtc_int_arg("minute", minute) ||
      !web_parse_rtc_int_arg("second", second) ||
      !web_parse_rtc_int_arg("volume", volume) ||
      !web_parse_rtc_int_arg("weekday_mask", weekday_mask) ||
      !s_server.hasArg("action") ||
      !s_server.hasArg("repeat")) {
    web_send_json_err("缺少闹钟参数");
    return false;
  }

  AppAlarmAction action = AppAlarmAction::RESUME_LAST;
  if (!app_alarm_action_from_key(s_server.arg("action"), action)) {
    web_send_json_err("闹钟动作参数无效");
    return false;
  }

  AppAlarmRepeatMode repeat_mode = AppAlarmRepeatMode::DAILY;
  if (!app_alarm_repeat_from_key(s_server.arg("repeat"), repeat_mode)) {
    web_send_json_err("闹钟重复模式无效");
    return false;
  }

  if (weekday_mask < 0 || weekday_mask > APP_ALARM_WEEKDAY_ALL) {
    web_send_json_err("闹钟星期参数无效");
    return false;
  }

  cfg.enabled = true;
  cfg.hour = static_cast<uint8_t>(hour);
  cfg.minute = static_cast<uint8_t>(minute);
  cfg.second = static_cast<uint8_t>(second);
  cfg.repeat_mode = repeat_mode;
  cfg.weekday_mask = static_cast<uint8_t>(weekday_mask) & APP_ALARM_WEEKDAY_ALL;
  cfg.volume = static_cast<uint8_t>(volume);
  cfg.action = action;

  if (!app_alarm_validate_config(cfg)) {
    web_send_json_err("闹钟参数无效；每周指定模式至少选择一天");
    return false;
  }

  return true;
}

static void web_handle_alarm_save()
{
  AppAlarmConfig cfg{};
  if (!web_parse_alarm_config_from_args(cfg)) {
    return;
  }

  if (!app_alarm_save_config(cfg)) {
    web_send_json_err(app_alarm_last_schedule_message(), 500);
    return;
  }

  web_send_alarm_status_json();
}

static void web_handle_alarm_disable()
{
  if (!app_alarm_disable()) {
    web_send_json_err(app_alarm_last_schedule_message(), 500);
    return;
  }
  web_send_alarm_status_json();
}

static void web_handle_alarm_delete()
{
  if (!app_alarm_delete()) {
    web_send_json_err(app_alarm_last_schedule_message(), 500);
    return;
  }
  web_send_alarm_status_json();
}

static void web_handle_rtc_alarm_test_1m() {
  if (!pcf85063_is_ready()) {
    web_send_json_err("RTC未就绪", 500);
    return;
  }
  if (!pcf85063_set_test_alarm_after_one_minute()) {
    web_send_json_err("RTC测试闹钟设置失败，请先校准RTC时间", 500);
    return;
  }
  web_send_rtc_status_json();
}

static bool web_parse_rtc_alarm_time_args(int& hour, int& minute, int& second) {
  if (!web_parse_rtc_int_arg("hour", hour) ||
      !web_parse_rtc_int_arg("minute", minute) ||
      !web_parse_rtc_int_arg("second", second)) {
    web_send_json_err("缺少RTC闹钟时间参数");
    return false;
  }

  if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
    web_send_json_err("RTC闹钟时间参数无效");
    return false;
  }

  return true;
}

static void web_handle_rtc_alarm_time() {
  if (!pcf85063_is_ready()) {
    web_send_json_err("RTC未就绪", 500);
    return;
  }

  int hour = 0;
  int minute = 0;
  int second = 0;
  if (!web_parse_rtc_alarm_time_args(hour, minute, second)) {
    return;
  }

  if (!pcf85063_set_alarm_time_of_day(static_cast<uint8_t>(hour),
                                      static_cast<uint8_t>(minute),
                                      static_cast<uint8_t>(second))) {
    web_send_json_err("RTC每日闹钟设置失败", 500);
    return;
  }

  LOGI("[Web] RTC每日闹钟已设置：%02d:%02d:%02d", hour, minute, second);
  web_send_rtc_status_json();
}

static void web_handle_rtc_alarm_power_time() {
  if (!pcf85063_is_ready()) {
    web_send_json_err("RTC未就绪", 500);
    return;
  }

  int hour = 0;
  int minute = 0;
  int second = 0;
  if (!web_parse_rtc_alarm_time_args(hour, minute, second)) {
    return;
  }

  if (!pcf85063_set_alarm_time_of_day(static_cast<uint8_t>(hour),
                                      static_cast<uint8_t>(minute),
                                      static_cast<uint8_t>(second))) {
    web_send_json_err("RTC指定时间开机测试闹钟设置失败", 500);
    return;
  }

  // 注意：app_power_request_save_and_shutdown() 目前只保存 reason 指针，
  // 所以这里必须传字符串常量，不能传局部 char 数组。
  LOGI("[Web] RTC指定时间开机测试已设置：%02d:%02d:%02d，准备保存并关机", hour, minute, second);
  app_power_request_save_and_shutdown("RTC指定时间开机测试", 1500);
  web_send_rtc_status_json();
}

static void web_handle_rtc_alarm_power_test_1m() {
  if (!pcf85063_is_ready()) {
    web_send_json_err("RTC未就绪", 500);
    return;
  }
  if (!pcf85063_set_alarm_after_seconds(60)) {
    web_send_json_err("RTC定时开机测试闹钟设置失败，请先校准RTC时间", 500);
    return;
  }

  app_power_request_save_and_shutdown("RTC 1分钟后开机测试", 1500);
  web_send_rtc_status_json();
}

static void web_handle_rtc_alarm_clear() {
  if (!pcf85063_is_ready()) {
    web_send_json_err("RTC未就绪", 500);
    return;
  }
  if (!pcf85063_disable_alarm()) {
    web_send_json_err("RTC闹钟清除失败", 500);
    return;
  }
  web_send_rtc_status_json();
}

static void web_handle_status() {
  web_snapshot_capture_into(s_web_status_snapshot);
  WebPlayerSnapshot& snap = s_web_status_snapshot;

  const WebRuntimeSettings ws = web_settings_get();

  const bool is_radio_cover = strcmp(snap.source_type, "radio") == 0;
  const bool allow_cover_fetch_now =
      snap.has_cover &&
      (is_radio_cover || !snap.is_playing || snap.cover_ready_for_web);

  const bool cover_loading =
      snap.has_cover &&
      !allow_cover_fetch_now &&
      !is_radio_cover &&
      snap.track_idx >= 0 &&
      !snap.rescanning;

  WebStatusJsonWriter json(s_web_status_json_chunk);
  if (!json.prepare()) {
    LOGW("[网页] /api/status 固定分块缓冲申请失败：需要=%uB",
         static_cast<unsigned>(kWebStatusJsonChunkReserve));
    web_send_json_err("状态响应缓冲区不足", 503);
    return;
  }
  if (!json.begin()) return;

  json.field_bool("ok", snap.ok);
  json.field_string("app_state", snap.app_state);
  json.field_string("app_state_label", snap.app_state_label);
  json.field_bool("rescanning", snap.rescanning);

  json.field_bool("is_playing", snap.is_playing);
  json.field_bool("is_paused", snap.is_paused);

  json.field_int("track_idx", snap.track_idx);
  json.field_string("title", snap.title);
  json.field_string("artist", snap.artist);
  json.field_string("album", snap.album);

  json.field_uint("play_ms", snap.play_ms);
  json.field_uint("total_ms", snap.total_ms);
  json.field_bool("seekable", snap.seekable);
  json.field_bool("seeking", snap.seeking);
  json.field_uint("seek_target_ms", snap.seek_target_ms);
  json.field_uint("seek_actual_ms", snap.seek_actual_ms);
  json.field_string("seek_result", snap.seek_result);
  json.field_string("seek_error", snap.seek_error);
  const WebUiControlSnapshot ui_control =
      web_ui_control_snapshot_get();

  json.field_uint("volume", snap.volume);
  json.field_bool("volume_locked", ui_control.volume_locked);
  json.field_uint("playback_revision", audio_service_playback_revision());
  json.field_uint("state_token", web_status_state_token(ui_control));

  json.field_string("mode", snap.mode);
  json.field_string("mode_label", snap.mode_label);
  json.field_string("view", snap.view);
  json.field_string("view_label", snap.view_label);

  json.field_int("display_pos", snap.display_pos);
  json.field_int("display_total", snap.display_total);
  json.field_int("current_group_idx", snap.current_group_idx);

  json.field_bool("can_cancel_scan", snap.can_cancel_scan);
  json.field_string("scan_action_label", snap.scan_action_label);

  json.field_bool("has_lyrics", snap.has_lyrics);
  json.field_bool("lyrics_loading", snap.lyrics_loading);
  json.field_string("current_lyric", snap.current_lyric);
  json.field_string("next_lyric", snap.next_lyric);
  json.field_string("following_lyric", snap.following_lyric);

  json.field_bool("show_next_lyric", snap.show_next_lyric);
  json.field_bool("show_cover", snap.show_cover);
  json.field_bool("web_cover_spin", snap.web_cover_spin);

  json.field_string("lyric_sync_mode", web_lyric_sync_mode_key(ws.lyric_sync_mode));
  json.field_string("lyric_sync_mode_label", web_lyric_sync_mode_label(ws.lyric_sync_mode));
  json.field_uint("lyric_wait_poll_threshold_ms",
                  web_lyric_sync_mode_threshold_ms(ws.lyric_sync_mode));

  json.field_uint("current_lyric_start_ms", snap.current_lyric_start_ms);
  json.field_uint("next_lyric_start_ms", snap.next_lyric_start_ms);
  json.field_uint("following_lyric_start_ms", snap.following_lyric_start_ms);
  json.field_uint("next_poll_ms", snap.next_poll_ms);

  json.field_bool("cover_loading", cover_loading);
  json.field_bool("has_cover", allow_cover_fetch_now);
  json.field_string("cover_rev", allow_cover_fetch_now ? snap.cover_rev.c_str() : "");
  json.field_string("cover_url", allow_cover_fetch_now ? snap.cover_url.c_str() : "");

  json.field_string("source_type", snap.source_type);
  json.field_bool("radio_active", snap.radio_active);
  json.field_int("radio_idx", snap.radio_idx);
  json.field_string("radio_name", snap.radio_name);
  json.field_string("radio_format", snap.radio_format);
  json.field_string("radio_region", snap.radio_region);
  json.field_string("radio_state", snap.radio_state);
  json.field_string("radio_error", snap.radio_error);
  json.field_string("radio_stream_title", snap.radio_stream_title);
  json.field_string("radio_backend", snap.radio_backend);
  json.field_uint("radio_bitrate", snap.radio_bitrate);

  json.field_int("net_track_idx", snap.net_track_idx);
  json.field_string("net_track_title", snap.net_track_title);
  json.field_string("net_track_state", snap.net_track_state);
  json.field_string("net_track_error", snap.net_track_error);

  if (!json.finish()) {
    web_abort_client();
  }
}

static void web_handle_status_check() {
  const WebUiControlSnapshot ui_control = web_ui_control_snapshot_get();
  AudioSeekStateSnapshot seek{};
  AppRescanState rescan{};
  const uint32_t state_token =
      web_status_state_token(ui_control, &seek, &rescan);

  bool has_client_token = false;
  uint32_t client_token = 0;
  if (s_server.hasArg("token")) {
    const String value = s_server.arg("token");
    char* end = nullptr;
    const unsigned long parsed = strtoul(value.c_str(), &end, 10);
    if (end && end != value.c_str() && *end == '\0') {
      client_token = static_cast<uint32_t>(parsed);
      has_client_token = true;
    }
  }

  const WebRuntimeSettings settings = web_settings_get();
  uint32_t next_check_ms = web_refresh_preset_poll_ms(settings.refresh_preset);
  if (rescan.rescanning) {
    next_check_ms = min<uint32_t>(next_check_ms, WEBCTRL_STATUS_POLL_SCAN_MS);
  }

  String json;
  json.reserve(360);
  json += "{\"ok\":true";
  json += ",\"changed\":";
  json += (!has_client_token || client_token != state_token) ? "true" : "false";
  json += ",\"state_token\":";
  json += String(static_cast<unsigned long>(state_token));
  json += ",\"playback_revision\":";
  json += String(static_cast<unsigned long>(audio_service_playback_revision()));
  json += ",\"play_ms\":";
  json += String(static_cast<unsigned long>(audio_get_play_ms()));
  json += ",\"total_ms\":";
  json += String(static_cast<unsigned long>(audio_get_total_ms()));
  json += ",\"is_playing\":";
  json += audio_service_is_playing() ? "true" : "false";
  json += ",\"is_paused\":";
  json += audio_service_is_paused() ? "true" : "false";
  json += ",\"rescanning\":";
  json += rescan.rescanning ? "true" : "false";
  json += ",\"seeking\":";
  json += seek.seeking ? "true" : "false";
  json += ",\"seek_target_ms\":";
  json += String(static_cast<unsigned long>(seek.target_ms));
  json += ",\"source_type\":\"";
  json += player_source_type_key(player_source_type_get());
  json += "\"";
  json += ",\"next_check_ms\":";
  json += String(static_cast<unsigned long>(next_check_ms));
  json += "}";

  web_send_no_cache_headers();
  s_server.send(200, "application/json; charset=utf-8", json);
}

static bool web_is_remote_image_url(const String& s) {
  return s.startsWith("http://") || s.startsWith("https://");
}

static String web_get_current_radio_logo(bool* out_is_remote = nullptr) {
  if (out_is_remote) *out_is_remote = false;

  const PlayerSourceState source = player_source_get();
  if (source.type != PlayerSourceType::NET_RADIO || source.radio_idx < 0) {
    return String();
  }

  String logo = source.radio_logo;
  if (logo.isEmpty()) {
    const RadioItem* item = radio_catalog_get((size_t)source.radio_idx);
    if (item && item->valid) {
      logo = item->logo;
    }
  }
  logo.trim();

  if (out_is_remote) {
    *out_is_remote = web_is_remote_image_url(logo);
  }
  return logo;
}

static void web_handle_radio_logo_current() {
  const auto source = player_source_get();
  const int radio_idx = source.radio_idx;

  bool is_remote = false;
  String logo = web_get_current_radio_logo(&is_remote);
  if (!logo.length()) {
    web_send_json_err("当前电台没有封面", 404);
    return;
  }

  const String radio_rev = web_make_radio_cover_rev(radio_idx, logo);
  const String etag = String("\"cover-radio-") + String(radio_idx) + "-" + radio_rev + "\"";

  if (is_remote) {
    if (web_if_none_match_hit(etag)) {
      LOGD("[网页] 电台台标 304：索引=%d 远程=1", radio_idx);
      web_send_not_modified(etag);
      return;
    }

    s_server.sendHeader("Cache-Control", "public, max-age=86400, immutable", true);
    s_server.sendHeader("ETag", etag, true);
    s_server.sendHeader("Location", logo, true);
    s_server.send(302, "text/plain; charset=utf-8", "");
    return;
  }

  if (web_if_none_match_hit(etag)) {
    LOGD("[网页] 电台台标 304：索引=%d 远程=0", radio_idx);
    web_send_not_modified(etag);
    return;
  }

  uint8_t* buf = nullptr;
  size_t len = 0;
  bool is_png = false;

  const bool ok = audio_service_fetch_cover(COVER_FILE_FALLBACK,
                                            "",
                                            logo.c_str(),
                                            0,
                                            0,
                                            &buf,
                                            &len,
                                            &is_png,
                                            true);
  if (!ok || !buf || len == 0) {
    if (buf) free(buf);
    web_send_json_err("电台封面读取失败", 500);
    return;
  }

  WiFiClient client = s_server.client();
  client.setTimeout(800);
  client.print("HTTP/1.1 200 OK\r\n");
  client.print("Content-Type: image/bmp\r\n");
  client.printf("Content-Length: %u\r\n", (unsigned)len);
  client.print("Cache-Control: public, max-age=86400, immutable\r\n");
  client.printf("ETag: %s\r\n", etag.c_str());
  client.print("Connection: close\r\n");
  client.print("\r\n");
  const size_t written = client.write(buf, len);
  client.flush();
  if (written != len) {
    LOGW("[网页] 电台 台标 发送写入不足 字节=%u/%u", (unsigned)written, (unsigned)len);
  }

  free(buf);
}

static void web_handle_cover_current() {
  const PlayerSourceState source = player_source_get();
  if (source.type == PlayerSourceType::NET_TRACK &&
      (s_server.hasArg("net") || player_state_current_index() < 0)) {
    int req_idx = source.net_track_idx;
    (void)web_parse_int_arg("idx", req_idx);

    CoverSource cover_source = COVER_NONE;
    uint32_t cover_offset = 0;
    uint32_t cover_size = 0;
    String cover_rev;
    if (!net_music_embedded_cover_get_current(req_idx,
                                              source.net_track_url,
                                              &cover_source,
                                              &cover_offset,
                                              &cover_size,
                                              &cover_rev)) {
      web_send_json_err("NAS 封面尚未就绪", 404);
      return;
    }

    const String etag = String("\"cover-net-") + String(req_idx) + "-" + cover_rev + "\"";
    if (web_if_none_match_hit(etag)) {
      LOGD("[网页] NAS 封面 304 idx=%d 版本=%s", req_idx, cover_rev.c_str());
      web_send_not_modified(etag);
      return;
    }

    uint8_t* buf = nullptr;
    size_t len = 0;
    const bool ok = web_cover_cache_copy_bmp(req_idx,
                                            cover_source,
                                            source.net_track_url.c_str(),
                                            "",
                                            cover_offset,
                                            cover_size,
                                            &buf,
                                            &len);
    if (!ok || !buf || len == 0) {
      if (buf) free(buf);
      web_send_json_err("NAS 封面缓存尚未就绪", 404);
      return;
    }

    LOGD("[网页] NAS 封面 BMP 命中 idx=%d 字节=%u", req_idx, (unsigned)len);

    WiFiClient client = s_server.client();
    client.setTimeout(800);
    client.print("HTTP/1.1 200 OK\r\n");
    client.print("Content-Type: image/bmp\r\n");
    client.printf("Content-Length: %u\r\n", (unsigned)len);
    client.print("Cache-Control: public, max-age=86400, immutable\r\n");
    client.printf("ETag: %s\r\n", etag.c_str());
    client.print("Connection: close\r\n");
    client.print("\r\n");
    const size_t written = client.write(buf, len);
    client.flush();
    if (written != len) {
      LOGW("[网页] NAS 封面 发送写入不足 idx=%d 字节=%u/%u", req_idx, (unsigned)written, (unsigned)len);
    }

    free(buf);
    return;
  }

  int cur = player_state_current_index();
  int req_track = -1;
  if (web_parse_int_arg("track", req_track)) cur = req_track;
  if (cur < 0) {
    web_send_json_err("当前没有曲目", 404);
    return;
  }

  TrackViewV3 v{};
  if (!storage_catalog_v3_get_track_view((uint32_t)cur, v, "/Music") || !v.valid) {
    web_send_json_err("读取曲目信息失败", 404);
    return;
  }
  if (v.cover_source == COVER_NONE || (v.cover_size == 0 && v.cover_path.length() == 0)) {
    web_send_json_err("当前曲目没有封面", 404);
    return;
  }

  const String cover_rev = web_make_track_cover_rev(v);
  const String etag = String("\"cover-track-") + String(cur) + "-" + cover_rev + "\"";
  if (web_if_none_match_hit(etag)) {
    LOGD("[网页] 封面 304 歌曲=%d 版本=%s", cur, cover_rev.c_str());
    web_send_not_modified(etag);
    return;
  }

    uint8_t* buf = nullptr;
    size_t len = 0;

    const bool ok = web_cover_cache_copy_bmp(cur,
                                            (CoverSource)v.cover_source,
                                            v.audio_path.c_str(),
                                            v.cover_path.c_str(),
                                            v.cover_offset,
                                            v.cover_size,
                                            &buf,
                                            &len);
    if (!ok || !buf || len == 0) {
    if (buf) free(buf);
    web_send_json_err("封面缓存尚未就绪", 404);
    return;
  }

  LOGD("[网页] 封面 BMP 命中 歌曲=%d 字节=%u", cur, (unsigned)len);

  WiFiClient client = s_server.client();
  client.setTimeout(800);
  client.print("HTTP/1.1 200 OK\r\n");
  client.print("Content-Type: image/bmp\r\n");
  client.printf("Content-Length: %u\r\n", (unsigned)len);
  client.print("Cache-Control: public, max-age=86400, immutable\r\n");
  client.printf("ETag: %s\r\n", etag.c_str());
  client.print("Connection: close\r\n");
  client.print("\r\n");
  const size_t written = client.write(buf, len);
  client.flush();
  if (written != len) {
    LOGW("[网页] 封面 发送写入不足 歌曲=%d 字节=%u/%u", cur, (unsigned)written, (unsigned)len);
  }

  free(buf);
}
static void web_handle_artists() {
  web_send_group_list_json(player_playlist_artist_groups(), false);
}
static void web_handle_albums() {
  web_send_group_list_json(player_playlist_album_groups(), true);
}

static void web_handle_artist_song_search() {
  web_send_no_cache_headers();

  String q = web_trim_copy(s_server.arg("q"));
  q.toLowerCase();

  String json;
  json.reserve(256);
  json += "{\"ok\":true,\"items\":[";

  if (q.length() == 0) {
    json += "]}";
    s_server.send(200, "application/json; charset=utf-8", json);
    return;
  }

  const MusicCatalogV3& cat = storage_catalog_v3();
  const auto& groups = player_playlist_artist_groups();

  bool first_item = true;

  for (int gi = 0; gi < (int)groups.size(); ++gi) {
    const PlaylistGroup& g = groups[(size_t)gi];

    int matched_count = 0;
    String matched_titles_text;

    for (size_t k = 0; k < g.track_indices.size(); ++k) {
      const int track_idx = (int)g.track_indices[k];
      if (!cat.tracks || track_idx < 0 || track_idx >= (int)cat.track_count) continue;

      const TrackRowV3& row = cat.tracks[(size_t)track_idx];
      const char* title_c = pool_str_v3(cat.pool, row.title_off);

      if (!web_str_icontains(title_c, q)) continue;

      matched_count++;

      if (matched_count <= 3) {
        if (matched_titles_text.length()) matched_titles_text += "、";
        matched_titles_text += String(title_c ? title_c : "");
      }
    }

    if (matched_count <= 0) continue;

    if (!first_item) json += ",";
    first_item = false;

    json += "{";
    json += "\"idx\":";
    json += String(gi);
    json += ",\"name\":\"";
    json += web_json_escape(playlist_group_name_string(cat, g));
    json += "\"";
    json += ",\"track_count\":";
    json += String((int)g.track_indices.size());
    json += ",\"matched_track_count\":";
    json += String(matched_count);
    web_json_append_match_titles(json, matched_titles_text);
    json += "}";
  }

  json += "]}";
  s_server.send(200, "application/json; charset=utf-8", json);
}

static void web_handle_album_song_search() {
  web_send_no_cache_headers();

  String q = web_trim_copy(s_server.arg("q"));
  q.toLowerCase();

  String json;
  json.reserve(256);
  json += "{\"ok\":true,\"items\":[";

  if (q.length() == 0) {
    json += "]}";
    s_server.send(200, "application/json; charset=utf-8", json);
    return;
  }

  const MusicCatalogV3& cat = storage_catalog_v3();
  const auto& groups = player_playlist_album_groups();

  bool first_item = true;

  for (int gi = 0; gi < (int)groups.size(); ++gi) {
    const PlaylistGroup& g = groups[(size_t)gi];

    int matched_count = 0;
    String matched_titles_text;

    for (size_t k = 0; k < g.track_indices.size(); ++k) {
      const int track_idx = (int)g.track_indices[k];
      if (!cat.tracks || track_idx < 0 || track_idx >= (int)cat.track_count) continue;

      const TrackRowV3& row = cat.tracks[(size_t)track_idx];
      const char* title_c = pool_str_v3(cat.pool, row.title_off);

      if (!web_str_icontains(title_c, q)) continue;

      matched_count++;

      if (matched_count <= 3) {
        if (matched_titles_text.length()) matched_titles_text += "、";
        matched_titles_text += String(title_c ? title_c : "");
      }
    }

    if (matched_count <= 0) continue;

    if (!first_item) json += ",";
    first_item = false;

    json += "{";
    json += "\"idx\":";
    json += String(gi);
    json += ",\"name\":\"";
    json += web_json_escape(playlist_group_name_string(cat, g));
    json += "\"";
    json += ",\"primary_artist\":\"";
    json += web_json_escape(playlist_group_primary_artist_string(cat, g));
    json += "\"";
    json += ",\"track_count\":";
    json += String((int)g.track_indices.size());
    json += ",\"matched_track_count\":";
    json += String(matched_count);
    web_json_append_match_titles(json, matched_titles_text);
    json += "}";
  }

  json += "]}";
  s_server.send(200, "application/json; charset=utf-8", json);
}

static void web_handle_nfc_bindings() {
  web_send_no_cache_headers();

  const int total = nfc_binding_count();

  String json;
  json.reserve(64 + total * 180);
  json += "{\"ok\":true,\"count\":";
  json += String(total);
  json += ",\"items\":[";

  for (int i = 0; i < total; ++i) {
    NfcBindingEntry entry;
    if (!nfc_binding_get(i, entry)) continue;

    if (i > 0) json += ",";

    json += "{";

    json += "\"index\":";
    json += String(i + 1);
    json += ",";

    json += "\"uid\":\"";
    json += web_json_escape(entry.uid);
    json += "\",";

    json += "\"type\":\"";
    json += web_json_escape(String(nfc_binding_type_to_cstr(entry.type)));
    json += "\",";

    json += "\"type_label\":\"";
    json += web_json_escape(String(web_nfc_type_label_cn(entry.type)));
    json += "\",";

    json += "\"display\":\"";
    json += web_json_escape(entry.display);
    json += "\",";

    json += "\"key\":\"";
    json += web_json_escape(entry.key);
    json += "\"";

    json += "}";
  }

  json += "]}";
  s_server.send(200, "application/json; charset=utf-8", json);
}
static void web_handle_nfc_binding_delete() {
  if (!web_require_player_state()) return;

  const String uid = web_trim_copy(s_server.arg("uid"));
  if (!uid.length()) {
    web_send_json_err("缺少 uid 参数");
    return;
  }

  NfcBindingEntry entry;
  if (!nfc_binding_find(uid, entry)) {
    web_send_json_err("绑定不存在", 404);
    return;
  }

  if (!nfc_binding_remove_and_save_safely(uid, nullptr, true)) {
    web_send_json_err("删除绑定失败", 500);
    return;
  }

  web_send_json_ok_simple("binding_deleted");
}
static void web_handle_nfc_binding_test_play() {
  if (!web_require_player_state()) return;

  const String uid = web_trim_copy(s_server.arg("uid"));
  if (!uid.length()) {
    web_send_json_err("缺少 uid 参数");
    return;
  }

  NfcBindingEntry entry;
  if (!nfc_binding_find(uid, entry)) {
    web_send_json_err("绑定不存在", 404);
    return;
  }

  if (!web_nfc_test_play_by_uid(uid)) {
    web_send_json_err("测试播放失败", 500);
    return;
  }

  web_send_json_ok_simple("已触发播放");
}
static void web_handle_artist_detail() {
  int idx = -1; if (!web_parse_int_arg("idx", idx)) { web_send_json_err("缺少 idx 参数"); return; }
  web_send_group_detail_json(player_playlist_artist_groups(), idx, false);
}
static void web_handle_album_detail() {
  int idx = -1; if (!web_parse_int_arg("idx", idx)) { web_send_json_err("缺少 idx 参数"); return; }
  web_send_group_detail_json(player_playlist_album_groups(), idx, true);
}
static void web_handle_artist_play() {
  if (!web_require_player_state()) return;
  int idx = -1; if (!web_parse_int_arg("idx", idx)) { web_send_json_err("缺少 idx 参数"); return; }
  if (!web_play_group_impl(false, idx)) { web_send_json_err("歌手分组播放失败", 500); return; }
  web_send_json_ok_simple("artist_play_started");
}
static void web_handle_album_play() {
  if (!web_require_player_state()) return;
  int idx = -1; if (!web_parse_int_arg("idx", idx)) { web_send_json_err("缺少 idx 参数"); return; }
  if (!web_play_group_impl(true, idx)) { web_send_json_err("专辑分组播放失败", 500); return; }
  web_send_json_ok_simple("album_play_started");
}
static void web_handle_track_play() {
  if (!web_require_player_state()) return;

  int track_idx = -1;
  if (!web_parse_int_arg("idx", track_idx)) {
    web_send_json_err("缺少 idx 参数");
    return;
  }
  if (track_idx < 0 || track_idx >= (int)storage_catalog_v3_track_count()) {
    web_send_json_err("曲目不存在", 404);
    return;
  }

  String mode = s_server.arg("mode");
  mode.toLowerCase();

  int group_idx = -1;
  web_parse_int_arg("group_idx", group_idx);

  if (mode == "artist") {
    const bool keep_random = control_mode_is_random(app_play_mode_get());
    const play_mode_t next_mode = keep_random ? PLAY_MODE_ARTIST_RND : PLAY_MODE_ARTIST_SEQ;
    (void)app_play_mode_set(next_mode, AppPlayModeChangeReason::WebControl);

    if (group_idx >= 0) player_playlist_set_current_group_idx(group_idx);
    else (void)player_playlist_align_group_context_for_track(track_idx, false);

  } else if (mode == "album") {
    const bool keep_random = control_mode_is_random(app_play_mode_get());
    const play_mode_t next_mode = keep_random ? PLAY_MODE_ALBUM_RND : PLAY_MODE_ALBUM_SEQ;
    (void)app_play_mode_set(next_mode, AppPlayModeChangeReason::WebControl);

    if (group_idx >= 0) player_playlist_set_current_group_idx(group_idx);
    else (void)player_playlist_align_group_context_for_track(track_idx, false);

  } else {
    // 单曲播放：不改变当前播放大类
    if (web_status_mode_is_artist() || web_status_mode_is_album()) {
      (void)player_playlist_align_group_context_for_track(track_idx, false);
    } else {
      player_playlist_set_current_group_idx(-1);
    }
  }

  player_playlist_force_rebuild();

  if (!player_play_idx_v3((uint32_t)track_idx, true, true)) {
    web_send_json_err("曲目播放失败", 500);
    return;
  }

  web_send_json_ok_simple("track_play_started");
}
static void web_handle_artist_bind_nfc() {
  if (!web_require_player_state()) return;

  int idx = -1;
  if (!web_parse_int_arg("idx", idx)) {
    web_send_json_err("缺少 idx 参数");
    return;
  }

  const auto& groups = player_playlist_artist_groups();
  if (idx < 0 || idx >= (int)groups.size()) {
    web_send_json_err("歌手分组不存在", 404);
    return;
  }

  const MusicCatalogV3& cat = storage_catalog_v3();

  NfcAdminTarget target{};
  target.type = NFC_ADMIN_TARGET_ARTIST;
  target.key = playlist_group_name_string(cat, groups[idx]);
  target.display = target.key;

  if (!app_request_enter_nfc_admin_with_target(target)) {
    web_send_json_err("进入 NFC 绑定失败", 500);
    return;
  }

  web_send_json_ok_simple("请到设备前刷卡并按播放键保存");
}
static void web_handle_album_bind_nfc() {
  if (!web_require_player_state()) return;

  int idx = -1;
  if (!web_parse_int_arg("idx", idx)) {
    web_send_json_err("缺少 idx 参数");
    return;
  }

  const auto& groups = player_playlist_album_groups();
  if (idx < 0 || idx >= (int)groups.size()) {
    web_send_json_err("专辑分组不存在", 404);
    return;
  }

  const MusicCatalogV3& cat = storage_catalog_v3();

  NfcAdminTarget target{};
  target.type = NFC_ADMIN_TARGET_ALBUM;
  target.key = playlist_group_display_string(cat, groups[idx]);
  target.display = target.key;

  if (!app_request_enter_nfc_admin_with_target(target)) {
    web_send_json_err("进入 NFC 绑定失败", 500);
    return;
  }

  web_send_json_ok_simple("请到设备前刷卡并按播放键保存");
}
static void web_handle_track_bind_nfc() {
  if (!web_require_player_state()) return;

  int track_idx = -1;
  if (!web_parse_int_arg("idx", track_idx)) {
    web_send_json_err("缺少 idx 参数");
    return;
  }

  if (track_idx < 0 || track_idx >= (int)storage_catalog_v3_track_count()) {
    web_send_json_err("曲目不存在", 404);
    return;
  }

  TrackViewV3 view;
  if (!storage_catalog_v3_get_track_view((uint32_t)track_idx, view)) {
    web_send_json_err("读取曲目信息失败", 500);
    return;
  }

  NfcAdminTarget target{};
  target.type = NFC_ADMIN_TARGET_TRACK;
  target.track_idx = track_idx;
  target.key = view.audio_path;
  target.display = view.title + " - " + view.artist;

  if (!app_request_enter_nfc_admin_with_target(target)) {
    web_send_json_err("进入 NFC 绑定失败", 500);
    return;
  }

  web_send_json_ok_simple("请到设备前刷卡并按播放键保存");
}
static void web_handle_radios_page() {
  web_send_no_cache_headers();
  s_server.send_P(200, "text/html; charset=utf-8", WEBCTRL_RADIOS_HTML);
}

static void web_handle_netmusic_page() {
  web_send_no_cache_headers();
  s_server.send_P(200, "text/html; charset=utf-8", WEBCTRL_NETMUSIC_HTML);
}

static int web_netmusic_hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// 仅用于网页显示曲库文件夹名称；按 UTF-8 字节解码 URL 中的 %XX。
static String web_netmusic_decode_url_segment(const String& encoded) {
  String decoded;
  (void)decoded.reserve(encoded.length());

  for (size_t i = 0; i < encoded.length(); ++i) {
    if (encoded[i] == '%' && i + 2 < encoded.length()) {
      const int hi = web_netmusic_hex_value(encoded[i + 1]);
      const int lo = web_netmusic_hex_value(encoded[i + 2]);
      if (hi >= 0 && lo >= 0) {
        decoded += static_cast<char>((hi << 4) | lo);
        i += 2;
        continue;
      }
    }
    decoded += encoded[i];
  }

  return decoded;
}

static String web_netmusic_source_display_name(const NetMusicSourceInfo& source) {
  String name = source.name;
  name.trim();
  if (name.length() && name != "NAS音乐") {
    return name;
  }

  String folder = source.relative_path;
  folder.replace('\\', '/');
  while (folder.endsWith("/")) {
    folder.remove(folder.length() - 1);
  }

  if (!folder.length()) {
    // 兼容旧版单列表配置：base 已直接指向歌曲文件夹时，从 URL 末段提取名称。
    folder = net_music_catalog_base_url();
    const int query_pos = folder.indexOf('?');
    if (query_pos >= 0) folder.remove(query_pos);
    const int fragment_pos = folder.indexOf('#');
    if (fragment_pos >= 0) folder.remove(fragment_pos);
    while (folder.endsWith("/")) {
      folder.remove(folder.length() - 1);
    }
  }

  const int slash = folder.lastIndexOf('/');
  if (slash >= 0) {
    folder = folder.substring(slash + 1);
  }
  folder = web_netmusic_decode_url_segment(folder);
  folder.trim();

  if (folder.length()) return folder;
  if (name.length()) return name;
  return String("NAS音乐");
}

static void web_append_netmusic_sources_json(String& json) {
  const uint8_t active_idx = net_music_catalog_active_source_index();
  NetMusicSourceInfo active_source{};
  String active_name = "NAS音乐";
  if (net_music_catalog_source_get(active_idx, &active_source)) {
    active_name = web_netmusic_source_display_name(active_source);
  }

  json += ",\"source_index\":";
  json += String((unsigned)active_idx);

  json += ",\"source_name\":\"";
  json += web_json_escape(active_name);
  json += "\"";

  json += ",\"sources\":[";
  bool first = true;
  const uint8_t count = net_music_catalog_source_count();
  for (uint8_t i = 0; i < count; ++i) {
    NetMusicSourceInfo source{};
    if (!net_music_catalog_source_get(i, &source) || !source.valid) {
      continue;
    }

    if (!first) json += ",";
    first = false;

    json += "{\"idx\":";
    json += String((unsigned)i);
    json += ",\"name\":\"";
    json += web_json_escape(web_netmusic_source_display_name(source));
    json += "\",\"path\":\"";
    json += web_json_escape(source.relative_path);
    json += "\",\"list\":\"";
    json += web_json_escape(source.list_name);
    json += "\"}";
  }
  json += "]";
}

struct WebNetMusicFocus {
  int idx = -1;
  const char* source = "none";
};

static WebNetMusicFocus web_resolve_netmusic_focus(uint32_t total) {
  WebNetMusicFocus focus{};
  const PlayerSourceState source = player_source_get();

  if (source.type == PlayerSourceType::NET_TRACK &&
      source.net_track_idx >= 0 &&
      static_cast<uint32_t>(source.net_track_idx) < total) {
    focus.idx = source.net_track_idx;
    focus.source = "playing";
    return focus;
  }

  const int snapshot_idx = player_snapshot_resolve_net_track_index(
      player_snapshot_net_track_index());
  if (snapshot_idx >= 0 && static_cast<uint32_t>(snapshot_idx) < total) {
    focus.idx = snapshot_idx;
    focus.source = "snapshot";
    return focus;
  }

  const int list_idx = player_list_select_saved_net_track_index();
  if (list_idx >= 0 && static_cast<uint32_t>(list_idx) < total) {
    focus.idx = list_idx;
    focus.source = "list";
    return focus;
  }

  if (total > 0) {
    focus.idx = 0;
    focus.source = "default";
  }
  return focus;
}

static void web_handle_radios() {
  web_send_radio_list_json();
}
static void web_handle_radio_play() {
  if (!web_require_player_state()) return;
  int idx = -1; if (!web_parse_int_arg("idx", idx)) { web_send_json_err("缺少 idx 参数"); return; }
  if (!web_radio_catalog_ensure_loaded()) { web_send_json_err("电台列表尚未加载", 500); return; }
  const RadioItem* item = radio_catalog_get((size_t)idx);
  if (!item || !item->valid) { web_send_json_err("电台不存在", 404); return; }
  if (!player_play_radio_index(idx)) { web_send_json_err("电台播放失败", 500); return; }
  web_send_json_ok_simple("已开始播放电台");
}
static void web_handle_radio_stop() {
  if (player_return_from_radio_to_local()) {
    web_send_json_ok_simple("已返回本地播放");
  } else {
    player_stop_radio();
    web_send_json_ok_simple("已停止电台");
  }
}
static void web_handle_netmusic() {
  if (!net_music_catalog_is_loaded()) {
    (void)net_music_catalog_load();
  }

  int offset = 0;
  int limit = 20;
  int detail = 0;

  web_parse_int_arg("offset", offset);
  web_parse_int_arg("limit", limit);
  web_parse_int_arg("detail", detail);

  if (offset < 0) offset = 0;
  if (limit <= 0) limit = 20;
  if (limit > 50) limit = 50;

  const uint32_t total = net_music_catalog_count();
  const WebNetMusicFocus focus = web_resolve_netmusic_focus(total);
  const bool locate_saved =
      s_server.hasArg("locate") && s_server.arg("locate") == "saved";
  if (locate_saved && focus.idx >= 0) {
    offset = (focus.idx / limit) * limit;
  }
  const uint32_t start = (uint32_t)offset;

  uint32_t end = start + (uint32_t)limit;
  if (start >= total) {
    end = start;
  } else if (end > total) {
    end = total;
  }

  String json;
  json.reserve(1024 + limit * 220);

  json += "{\"ok\":";
  json += net_music_catalog_is_loaded() ? "true" : "false";

  json += ",\"total\":";
  json += String((unsigned long)total);

  json += ",\"offset\":";
  json += String(offset);

  json += ",\"limit\":";
  json += String(limit);

  json += ",\"focus_idx\":";
  json += String(focus.idx);
  json += ",\"focus_source\":\"";
  json += focus.source;
  json += "\"";

  const PlayerSourceState current_source = player_source_get();
  const int playing_idx = current_source.type == PlayerSourceType::NET_TRACK
      ? current_source.net_track_idx
      : -1;
  json += ",\"playing_idx\":";
  json += String(playing_idx);

  json += ",\"base\":\"";
  json += web_json_escape(net_music_catalog_base_url());
  json += "\"";

  web_append_netmusic_sources_json(json);

  json += ",\"error\":\"";
  json += web_json_escape(net_music_catalog_error());
  json += "\"";

  json += ",\"items\":[";

  bool first = true;

  for (uint32_t i = start; i < end; ++i) {
    NetMusicItem item{};
    if (!net_music_catalog_get(i, &item) || !item.valid) {
      continue;
    }

    if (!first) {
      json += ",";
    }
    first = false;

    json += "{\"idx\":";
    json += String((unsigned long)i);

    json += ",\"title\":\"";
    json += web_json_escape(item.title);
    json += "\"";

    json += ",\"artist\":\"";
    json += web_json_escape(item.artist);
    json += "\"";

    json += ",\"album\":\"";
    json += web_json_escape(item.album);
    json += "\"";

    json += ",\"format\":\"";
    json += web_json_escape(item.format);
    json += "\"";

    json += ",\"duration_ms\":";
    json += String((unsigned long)item.duration_ms);

    if (detail != 0) {
      json += ",\"path\":\"";
      json += web_json_escape(item.encoded_path);
      json += "\"";
    }

    json += "}";
  }

  json += "]}";

  web_send_no_cache_headers();
  s_server.send(200, "application/json; charset=utf-8", json);
}

static void web_handle_netmusic_search() {
  if (!net_music_catalog_is_loaded()) {
    (void)net_music_catalog_load();
  }

  String q = s_server.hasArg("q") ? s_server.arg("q") : String();
  q.trim();

  int limit = 50;
  int detail = 0;
  web_parse_int_arg("limit", limit);
  web_parse_int_arg("detail", detail);

  if (limit <= 0) limit = 20;
  if (limit > 50) limit = 50;

  if (!q.length()) {
    web_send_no_cache_headers();
    s_server.send(200,
                  "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"empty_query\",\"matched\":0,\"items\":[]}");
    return;
  }

  std::vector<NetMusicSearchHit> hits;
  hits.reserve((size_t)limit);

  const uint32_t matched =
      net_music_catalog_search(q, (uint16_t)limit, &hits);

  String json;
  json.reserve(1024 + hits.size() * 240);

  json += "{\"ok\":";
  json += net_music_catalog_is_loaded() ? "true" : "false";

  json += ",\"query\":\"";
  json += web_json_escape(q);
  json += "\"";

  json += ",\"matched\":";
  json += String((unsigned long)matched);

  json += ",\"returned\":";
  json += String((unsigned long)hits.size());

  json += ",\"limit\":";
  json += String(limit);

  web_append_netmusic_sources_json(json);

  json += ",\"error\":\"";
  json += web_json_escape(net_music_catalog_error());
  json += "\"";

  json += ",\"items\":[";

  bool first = true;
  for (const auto& hit : hits) {
    const NetMusicItem& item = hit.item;

    if (!first) json += ",";
    first = false;

    json += "{\"idx\":";
    json += String((unsigned long)hit.idx);

    json += ",\"title\":\"";
    json += web_json_escape(item.title);
    json += "\"";

    json += ",\"artist\":\"";
    json += web_json_escape(item.artist);
    json += "\"";

    json += ",\"album\":\"";
    json += web_json_escape(item.album);
    json += "\"";

    json += ",\"format\":\"";
    json += web_json_escape(item.format);
    json += "\"";

    json += ",\"duration_ms\":";
    json += String((unsigned long)item.duration_ms);

    if (detail != 0) {
      json += ",\"path\":\"";
      json += web_json_escape(item.encoded_path);
      json += "\"";
    }

    json += "}";
  }

  json += "]}";

  web_send_no_cache_headers();
  s_server.send(200, "application/json; charset=utf-8", json);
}

static void web_handle_netmusic_source_select() {
  if (!web_require_player_state()) return;

  int requested_idx = -1;
  if (!web_parse_int_arg("idx", requested_idx)) {
    web_send_json_err("缺少 idx 参数");
    return;
  }

  if (net_music_catalog_source_count() == 0 &&
      !net_music_catalog_load_base()) {
    web_send_json_err("NAS 曲库源配置尚未加载", 500);
    return;
  }

  if (requested_idx < 0 ||
      requested_idx >= (int)net_music_catalog_source_count()) {
    web_send_json_err("NAS 歌曲文件夹不存在", 404);
    return;
  }

  const uint8_t old_idx = net_music_catalog_active_source_index();
  const uint8_t new_idx = static_cast<uint8_t>(requested_idx);
  if (new_idx != old_idx) {
    // 与实体菜单切换流程保持一致：先保存旧曲库位置，再释放旧列表和播放状态。
    (void)player_list_select_flush_persistent_state();
    (void)player_snapshot_save_to_nvs();

    if (player_source_type_get() == PlayerSourceType::NET_TRACK) {
      player_stop_net_track();
    }

    if (!net_music_catalog_select_source(new_idx)) {
      web_send_json_err("NAS 歌曲文件夹切换失败", 500);
      return;
    }

    // 每个曲库使用独立的播放快照，不继承上一文件夹的歌曲索引。
    (void)player_snapshot_reload_net_context_for_active_source();
  }

  if (!net_music_catalog_load()) {
    const String error = net_music_catalog_error();
    String message = "NAS 歌曲列表加载失败";
    if (error.length()) {
      message += "：";
      message += error;
    }
    web_send_json_err(message.c_str(), 500);
    return;
  }

  const int total = (int)net_music_catalog_count();
  const WebNetMusicFocus focus =
      web_resolve_netmusic_focus(static_cast<uint32_t>(total));
  const int focus_idx = focus.idx;
  const char* focus_source = focus.source;

  String json;
  json.reserve(720);
  json += "{\"ok\":true,\"message\":\"已切换 NAS 歌曲文件夹\"";
  json += ",\"total\":";
  json += String((unsigned long)total);
  json += ",\"focus_idx\":";
  json += String(focus_idx);
  json += ",\"focus_source\":\"";
  json += focus_source;
  json += "\"";
  web_append_netmusic_sources_json(json);
  json += "}";

  web_send_no_cache_headers();
  s_server.send(200, "application/json; charset=utf-8", json);
}

static void web_handle_netmusic_play() {
  if (!web_require_player_state()) return;

  int idx = -1;
  if (!web_parse_int_arg("idx", idx)) {
    web_send_json_err("缺少 idx 参数");
    return;
  }

  if (!net_music_catalog_is_loaded()) {
    (void)net_music_catalog_load();
  }

  const int count = (int)net_music_catalog_count();
  if (idx < 0 || idx >= count) {
    web_send_json_err("网络歌曲不存在", 404);
    return;
  }

  if (!player_play_net_track_index(idx)) {
    web_send_json_err("网络歌曲播放失败", 500);
    return;
  }

  web_send_json_ok_simple("已开始播放 NAS 歌曲");
}

static void web_handle_netmusic_prev() {
  if (!web_require_player_state()) return;

  const PlayerSourceState source = player_source_get();
  if (source.type != PlayerSourceType::NET_TRACK) {
    web_send_json_err("当前不是 NAS 播放");
    return;
  }

  player_prev_track();
  web_send_json_ok_simple("NAS 上一首");
}

static void web_handle_netmusic_next() {
  if (!web_require_player_state()) return;

  const PlayerSourceState source = player_source_get();
  if (source.type != PlayerSourceType::NET_TRACK) {
    web_send_json_err("当前不是 NAS 播放");
    return;
  }

  player_next_track();
  web_send_json_ok_simple("NAS 下一首");
}

static void web_handle_netmusic_toggle() {
  if (!web_require_player_state()) return;

  const PlayerSourceState source = player_source_get();
  if (source.type != PlayerSourceType::NET_TRACK) {
    web_send_json_err("当前不是 NAS 播放");
    return;
  }

  player_toggle_play(PlayerToggleTrigger::Web);
  web_send_json_ok_simple("NAS 播放 / 暂停");
}

static void web_handle_netmusic_mode() {
  if (!web_require_player_state()) return;

  if (!player_net_track_toggle_order_random()) {
    web_send_json_err("NAS 顺序 / 随机切换失败");
    return;
  }

  web_send_json_ok_simple("NAS 播放模式已切换");
}

static void web_handle_netmusic_return_local() {
  if (!web_require_player_state()) return;

  if (!player_return_from_network_to_local()) {
    web_send_json_err("返回本地播放失败");
    return;
  }

  web_send_json_ok_simple("已返回本地播放");
}

static bool web_parse_seek_ms_arg(uint32_t& out_ms)
{
  String value = s_server.arg("ms");
  if (value.length() == 0) value = s_server.arg("value");
  value.trim();
  if (value.length() == 0) return false;

  char* end = nullptr;
  const uint64_t parsed = strtoull(value.c_str(), &end, 10);
  if (!end || *end != '\0' || parsed > UINT32_MAX) return false;
  out_ms = static_cast<uint32_t>(parsed);
  return true;
}

static void web_handle_seek()
{
  if (!web_require_player_state()) return;
  if (player_source_type_get() == PlayerSourceType::NET_RADIO) {
    web_send_json_err("网络电台不支持进度跳转");
    return;
  }

  uint32_t target_ms = 0;
  if (!web_parse_seek_ms_arg(target_ms)) {
    web_send_json_err("缺少或无效的跳转参数 ms");
    return;
  }
  if (!audio_service_is_seekable() || audio_get_total_ms() == 0) {
    web_send_json_err("当前音源暂不支持进度跳转");
    return;
  }

  uint32_t request_id = 0;
  if (!audio_service_seek_ms_async(target_ms, &request_id)) {
    web_send_json_err("音频任务繁忙，跳转请求未入队", 503);
    return;
  }

  web_send_no_cache_headers();
  String json = "{\"ok\":true,\"message\":\"seek_started\",\"request_id\":";
  json += request_id;
  json += ",\"target_ms\":";
  json += target_ms;
  json += "}";
  s_server.send(200, "application/json; charset=utf-8", json);
}

static void web_handle_playpause() { if (!web_require_player_state()) return; player_toggle_play(PlayerToggleTrigger::Web); web_send_json_ok_simple(); }
static void web_handle_next() { if (!web_require_player_state()) return; player_next_track(); web_send_json_ok_simple(); }
static void web_handle_prev() { if (!web_require_player_state()) return; player_prev_track(); web_send_json_ok_simple(); }
static void web_handle_mode_toggle() { if (!web_require_player_state()) return; player_toggle_random(); web_send_json_ok_simple(); }
static void web_handle_mode_category() { if (!web_require_player_state()) return; player_cycle_mode_category(); web_send_json_ok_simple(); }
static void web_handle_view_toggle() { if (!web_require_player_state()) return; ui_toggle_view(); web_send_json_ok_simple(); }
static bool web_parse_volume_arg(uint8_t& out_value) { String s = s_server.arg("value"); if (s.length()==0) s = s_server.arg("v"); if (s.length()==0) return false; int v=s.toInt(); if (v<0) v=0; if (v>100) v=100; out_value=(uint8_t)v; return true; }
static void web_handle_volume() {
  if (g_app_state != STATE_PLAYER) { web_send_json_err("当前不在播放器状态"); return; }
  uint8_t v = 0; if (!web_parse_volume_arg(v)) { web_send_json_err("缺少音量参数 value"); return; }
  (void)audio_output_route_set_user_volume(v); ui_volume_key_pressed(); web_send_json_ok_simple();
}

static bool web_parse_lock_value(bool& out_value, bool current_value) {
  String s = s_server.arg("value");
  if (s.length() == 0) s = s_server.arg("locked");
  if (s.length() == 0) s = s_server.arg("v");

  if (s.length() == 0) {
    out_value = !current_value;
    return true;
  }

  out_value = web_parse_bool(s, current_value);
  return true;
}

static void web_send_volume_lock_state_json() {
  const WebUiControlSnapshot ui_control =
      web_ui_control_snapshot_get();

  web_send_no_cache_headers();

  String json = "{\"ok\":true";
  json += ",\"volume_locked\":";
  json += (ui_control.volume_locked ? "true" : "false");
  json += "}";

  s_server.send(200, "application/json; charset=utf-8", json);
}

static void web_handle_volume_lock() {
  const WebUiControlSnapshot ui_control =
      web_ui_control_snapshot_get();

  bool v = false;
  if (!web_parse_lock_value(v, ui_control.volume_locked)) {
    web_send_json_err("音量锁参数错误");
    return;
  }

  web_ui_volume_locked_set(v);
  web_send_volume_lock_state_json();
}

static void web_handle_state_save() {
  if (!web_require_player_state()) return;

  // 显式保存会同时写入本地与 NAS 两套快照；电台播放时保留最近一次音乐状态。
  if (!player_snapshot_save_to_nvs()) {
    web_send_json_err("保存播放器状态失败", 500);
    return;
  }

  web_send_json_ok_simple("player_state_saved");
}

static void web_handle_scan() {
  const AppRescanState rescan = app_rescan_state_get();
  if (rescan.rescanning) {
    if (rescan.committed) {
      web_send_json_ok_simple("rescan_finishing");
      return;
    }
    if (!app_request_cancel_rescan()) {
      web_send_json_err("当前没有正在进行的重扫");
      return;
    }
    web_send_json_ok_simple(rescan.abort_requested
        ? "rescan_cancel_pending"
        : "rescan_cancel_requested");
    return;
  }
  AppRescanMode requested_mode = AppRescanMode::FastIncremental;
  if (s_server.hasArg("mode")) {
    String mode = s_server.arg("mode");
    mode.trim();
    mode.toLowerCase();
    if (mode == "ultra" || mode == "directory") {
      requested_mode = AppRescanMode::UltraFastIncremental;
    } else if (mode == "strict") {
      requested_mode = AppRescanMode::StrictIncremental;
    } else if (mode == "full") {
      requested_mode = AppRescanMode::Full;
    } else if (mode != "fast" && mode != "incremental") {
      web_send_json_err("重扫模式无效：ultra/fast/strict/full");
      return;
    }
  }

  if (!app_request_start_rescan(requested_mode)) {
    web_send_json_err("当前状态不允许开始重扫");
    return;
  }

  web_send_json_ok_simple(
      requested_mode == AppRescanMode::Full
          ? "rescan_full_started"
          : (requested_mode == AppRescanMode::StrictIncremental
                 ? "rescan_strict_started"
                 : (requested_mode == AppRescanMode::UltraFastIncremental
                        ? "rescan_ultra_started"
                        : "rescan_fast_started")));
}

static void web_setup_routes() {
  s_server.on("/", HTTP_GET, web_handle_root);
  s_server.on("/artists", HTTP_GET, web_handle_artists_page);
  s_server.on("/albums", HTTP_GET, web_handle_albums_page);
  s_server.on("/nfc", HTTP_GET, web_handle_nfc_page);
  s_server.on("/radios", HTTP_GET, web_handle_radios_page);
  s_server.on("/netmusic", HTTP_GET, web_handle_netmusic_page);
  s_server.on("/settings", HTTP_GET, web_handle_settings_page);
  s_server.on("/web-feedback.js", HTTP_GET, web_handle_feedback_js);
  s_server.on("/favicon.ico", HTTP_GET, web_handle_favicon);
  s_server.on("/api/status", HTTP_GET, web_handle_status);
  s_server.on("/api/status/check", HTTP_GET, web_handle_status_check);
  s_server.on("/api/artists", HTTP_GET, web_handle_artists);
  s_server.on("/api/albums", HTTP_GET, web_handle_albums);
  s_server.on("/api/artist/search_song", HTTP_GET, web_handle_artist_song_search);
  s_server.on("/api/album/search_song", HTTP_GET, web_handle_album_song_search);
  s_server.on("/api/radios", HTTP_GET, web_handle_radios);
  s_server.on("/api/netmusic", HTTP_GET, web_handle_netmusic);
  s_server.on("/api/netmusic/search", HTTP_GET, web_handle_netmusic_search);
  s_server.on("/api/netmusic/source", HTTP_POST, web_handle_netmusic_source_select);
  s_server.on("/api/artist/detail", HTTP_GET, web_handle_artist_detail);
  s_server.on("/api/album/detail", HTTP_GET, web_handle_album_detail);
  s_server.on("/api/settings", HTTP_GET, web_handle_settings_get);
  s_server.on("/api/settings", HTTP_POST, web_handle_settings_post);
  s_server.on("/api/config/wifi", HTTP_GET, web_handle_wifi_config_get);
  s_server.on("/api/config/wifi/save", HTTP_POST, web_handle_wifi_config_save);
  s_server.on("/api/config/wifi/apply", HTTP_POST, web_handle_wifi_config_apply);
  s_server.on("/api/config/radios", HTTP_GET, web_handle_radio_config_get);
  s_server.on("/api/config/radios/save", HTTP_POST, web_handle_radio_config_save);
  s_server.on("/api/config/nas", HTTP_GET, web_handle_nas_config_get);
  s_server.on("/api/config/nas/save", HTTP_POST, web_handle_nas_config_save);
  s_server.on("/api/config/nas/apply", HTTP_POST, web_handle_nas_config_apply);
  s_server.on("/api/config/assets", HTTP_GET, web_handle_system_assets_get);
  s_server.on("/api/config/assets/default-cover",
              HTTP_POST,
              web_handle_default_cover_upload_complete,
              web_handle_default_cover_upload_stream);
  s_server.on("/api/config/assets/net-cover-loading",
              HTTP_POST,
              web_handle_net_cover_loading_upload_complete,
              web_handle_net_cover_loading_upload_stream);
  s_server.on("/api/config/radios/logo",
              HTTP_POST,
              web_handle_radio_logo_upload_complete,
              web_handle_radio_logo_upload_stream);
  s_server.on("/api/audio-output/status", HTTP_GET, web_handle_audio_output_status);
  s_server.on("/api/audio-output/route", HTTP_POST, web_handle_audio_output_route);
  s_server.on("/api/audio-output/amp-mute", HTTP_POST, web_handle_audio_output_amp_mute);
  s_server.on("/api/audio-output/bluetooth/query", HTTP_POST, web_handle_audio_output_bt_query);
  s_server.on("/api/audio-output/bluetooth/volume", HTTP_POST, web_handle_audio_output_bt_volume);
  s_server.on("/api/audio-output/bluetooth/pair", HTTP_POST, web_handle_audio_output_bt_pair);
  s_server.on("/api/audio-output/bluetooth/restart", HTTP_POST, web_handle_audio_output_bt_restart);
  s_server.on("/api/system/diagnostics", HTTP_GET, web_handle_system_diagnostics);
  s_server.on("/api/rtc/status", HTTP_GET, web_handle_rtc_status);
  s_server.on("/api/rtc/time", HTTP_POST, web_handle_rtc_set_time);
  s_server.on("/api/alarm/status", HTTP_GET, web_handle_alarm_status);
  s_server.on("/api/alarm/save", HTTP_POST, web_handle_alarm_save);
  s_server.on("/api/alarm/disable", HTTP_POST, web_handle_alarm_disable);
  s_server.on("/api/alarm/delete", HTTP_POST, web_handle_alarm_delete);
  s_server.on("/api/cover/current", HTTP_GET, web_handle_cover_current);
  s_server.on("/api/radio/logo/current", HTTP_GET, web_handle_radio_logo_current);
  s_server.on("/api/artist/play", HTTP_POST, web_handle_artist_play);
  s_server.on("/api/album/play", HTTP_POST, web_handle_album_play);
  s_server.on("/api/track/play", HTTP_POST, web_handle_track_play);
  s_server.on("/api/nfc/bindings", HTTP_GET, web_handle_nfc_bindings);
  s_server.on("/api/nfc/binding/delete", HTTP_POST, web_handle_nfc_binding_delete);
  s_server.on("/api/nfc/binding/test_play", HTTP_POST, web_handle_nfc_binding_test_play);
  s_server.on("/api/artist/bind_nfc", HTTP_POST, web_handle_artist_bind_nfc);
  s_server.on("/api/album/bind_nfc", HTTP_POST, web_handle_album_bind_nfc);
  s_server.on("/api/track/bind_nfc", HTTP_POST, web_handle_track_bind_nfc);
  s_server.on("/api/radio/play", HTTP_POST, web_handle_radio_play);
  s_server.on("/api/radio/stop", HTTP_POST, web_handle_radio_stop);
  s_server.on("/api/netmusic/play", HTTP_GET, web_handle_netmusic_play);
  s_server.on("/api/netmusic/play", HTTP_POST, web_handle_netmusic_play);
  s_server.on("/api/netmusic/prev", HTTP_POST, web_handle_netmusic_prev);
  s_server.on("/api/netmusic/next", HTTP_POST, web_handle_netmusic_next);
  s_server.on("/api/netmusic/toggle", HTTP_POST, web_handle_netmusic_toggle);
  s_server.on("/api/netmusic/mode", HTTP_POST, web_handle_netmusic_mode);
  s_server.on("/api/netmusic/return-local", HTTP_POST, web_handle_netmusic_return_local);
  s_server.on("/api/playpause", HTTP_POST, web_handle_playpause);
  s_server.on("/api/seek", HTTP_POST, web_handle_seek);
  s_server.on("/api/next", HTTP_POST, web_handle_next);
  s_server.on("/api/prev", HTTP_POST, web_handle_prev);
  s_server.on("/api/mode/toggle", HTTP_POST, web_handle_mode_toggle);
  s_server.on("/api/mode/category", HTTP_POST, web_handle_mode_category);
  s_server.on("/api/view/toggle", HTTP_POST, web_handle_view_toggle);
  s_server.on("/api/volume", HTTP_POST, web_handle_volume);
  s_server.on("/api/ui/volume_lock", HTTP_POST, web_handle_volume_lock);
  s_server.on("/api/state/save", HTTP_POST, web_handle_state_save);
  s_server.on("/api/scan", HTTP_POST, web_handle_scan);
  s_server.onNotFound([](){ web_send_json_err("not_found", 404); });
}

bool web_server_switch_wifi_from_config()
{
#if WEBCTRL_ENABLED
  if (!s_wifi_enabled) {
    LOGI("[网页] WiFi 当前关闭，切换 WiFi 将先启用 WiFi");
    web_wifi_set_enabled(true);
    quick_menu_request_refresh();
    return true;
  }

  std::vector<WebWifiNetwork> nets;
  String hostname;
  if (!web_load_wifi_config(nets, hostname) || nets.empty()) {
    LOGW("[网页] 切换 WiFi 失败：没有可用配置");
    quick_menu_request_refresh();
    return false;
  }

  const String current_ssid = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String("");
  int current_index = -1;
  for (int i = 0; i < (int)nets.size(); ++i) {
    if (current_ssid.length() && nets[i].ssid == current_ssid) {
      current_index = i;
      break;
    }
  }

  LOGI("[网页] 切换 WiFi：当前=%s 配置数量=%d",
       current_ssid.length() ? current_ssid.c_str() : "-",
       (int)nets.size());

  // 切换 WiFi 前先停掉网络音频，避免 AudioTask 正在 WiFiClient::read() 时断网。
  web_stop_network_audio_before_wifi_down("switch WiFi");

  const int total = (int)nets.size();
  const int start = current_index >= 0 ? ((current_index + 1) % total) : 0;

  for (int step = 0; step < total; ++step) {
    const int idx = (start + step) % total;

    // 多个配置时，优先跳过当前 SSID；只有一个配置时允许重连当前 SSID。
    if (total > 1 && current_ssid.length() && nets[idx].ssid == current_ssid) {
      continue;
    }

    LOGI("[网页] 尝试切换到 WiFi：%s", nets[idx].ssid.c_str());
    if (web_try_connect_one(nets[idx], hostname)) {
      s_ap_mode = false;

      if (!s_started) {
        web_server_start_async();
      }

      quick_menu_request_refresh();
      return true;
    }
  }

  LOGW("[网页] 切换 WiFi 失败，恢复 AP 兜底模式");
  web_start_ap_fallback();
  quick_menu_request_refresh();
  return false;
#else
  return false;
#endif
}

bool web_server_retry_sta_from_config()
{
#if WEBCTRL_ENABLED
  if (!s_wifi_enabled) {
    LOGW("[网页] 跳过 STA 重试：WiFi 已关闭");
    return false;
  }

  if (!s_started) {
    web_server_start();
    return s_ready && !s_ap_mode;
  }

  if (!s_ready) {
    return false;
  }

  // 如果已经是 STA 且连接正常，不重复切换。
  if (!s_ap_mode && WiFi.status() == WL_CONNECTED) {
    LOGD("[网页] STA 已连接，IP=%s", WiFi.localIP().toString().c_str());
    quick_menu_request_refresh();
    return true;
  }

  LOGD("[网页] 根据配置重试 STA 连接");

  const bool ok = web_try_connect_sta_from_config();

  if (ok) {
    // s_server 已经 begin 过，WiFi 从 AP 切 STA 后一般不需要重新注册路由。
    LOGI("[网页] 已切换到 STA，IP=%s", WiFi.localIP().toString().c_str());
    quick_menu_request_refresh();
    return true;
  }

  // 注意：web_try_connect_sta_from_config 失败后会 WiFi.disconnect，
  // 如果不重新拉起 AP，网页控制入口会丢失。
  LOGW("[网页] STA 重试失败，恢复 AP 兜底模式");
  web_start_ap_fallback();
  return false;
#else
  return false;
#endif
}

static void web_start_task_entry(void* arg)
{
    (void)arg;

    // 先让播放器、I2S、功放时序稳定下来。
    // 避免 WiFi association/DHCP 正好撞上开机起播。
    vTaskDelay(pdMS_TO_TICKS(3000));

    web_server_start();

    web_start_task_finish(xTaskGetCurrentTaskHandle());
    vTaskDelete(nullptr);
}

void web_server_start() {
  #if WEBCTRL_ENABLED
    // 开机启动 Web/WiFi 前，先读取 NVS 设置。
    // 如果用户上次在菜单中关闭了 WiFi，这里直接跳过，不扫网、不启动 AP。
    web_settings_load();
    s_wifi_enabled = web_settings_get().wifi_enabled;

    if (!s_wifi_enabled) {
      LOGW("[网页] 跳过启动：NVS 设置中 WiFi 已关闭");
      WiFi.softAPdisconnect(true);
      WiFi.disconnect(true, true);
      WiFi.mode(WIFI_OFF);
      s_started = false;
      s_ready = false;
      s_ap_mode = false;
      return;
    }

        if (s_started) return;

    // s_started 只表示 WebServer 已经完成 begin()。
    // 不能在联网成功前提前置 true，否则 STA 和 AP 都启动失败后，后续重试会被永久跳过。
    s_ready = false;
    WiFi.persistent(false);
    WiFi.setAutoReconnect(false);
    web_settings_load();

    const bool net_ok =
        web_try_connect_sta_from_config() ||
        web_start_ap_fallback();

    if (!net_ok) {
      LOGE("[网页] 网络启动失败，保留后续重试能力");
      s_started = false;
      s_ready = false;
      s_ap_mode = false;
      return;
    }

    static const char* kHeaderKeys[] = { "If-None-Match" };
    s_server.collectHeaders(kHeaderKeys, 1);

    web_setup_routes();
    s_server.begin();

    s_started = true;
    s_ready = true;
    LOGI("[网页] 服务已启动：http://%s/", web_ip_string().c_str());
  #else
    s_started = true; s_ready = false;
  #endif
}

void web_server_start_async()
{
#if WEBCTRL_ENABLED
    // 开机启动 Web/WiFi 前，先读取 NVS 设置。
    // 如果用户上次在菜单中关闭了 WiFi，这里直接跳过，不扫网、不启动 AP。
    web_settings_load();
    s_wifi_enabled = web_settings_get().wifi_enabled;

    if (!s_wifi_enabled) {
        LOGD("[网页] NVS 设置中 WiFi 已关闭，Web 服务不启动");
        WiFi.softAPdisconnect(true);
        WiFi.disconnect(true, true);
        WiFi.mode(WIFI_OFF);
        s_started = false;
        s_ready = false;
        s_ap_mode = false;
        return;
    }

    if (s_started || !web_start_task_try_reserve()) {
        return;
    }

    TaskHandle_t created_task = nullptr;
    const BaseType_t ok = xTaskCreatePinnedToCore(
        web_start_task_entry,
        "WebStart",
        6144,
        nullptr,
        1,
        &created_task,
        1
    );

    if (ok != pdPASS || !created_task) {
        web_start_task_finish(nullptr);
        LOGE("[网页] 创建异步启动任务失败：返回值=%ld", (long)ok);
    } else {
        web_start_task_publish_handle(created_task);
        LOGD("[网页] 异步启动任务已创建");
    }
#else
    web_server_start();
#endif
}

void web_server_poll()
{
#if WEBCTRL_ENABLED
    if (!s_ready) return;
    s_server.handleClient();
    web_process_pending_wifi_config_apply();
    web_process_pending_nas_config_apply();
#endif
}

bool web_server_started()
{
    return s_started;
}

bool web_server_ready()
{
    return s_ready;
}
