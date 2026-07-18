#include "web/web_server.h"

#include <WiFi.h>
#include <WebServer.h>
#include <SdFat.h>
#include <vector>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

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
#include "storage/storage_groups_v3.h"
#include "ui/ui.h"
#include "menu/quick_menu.h"
#include "utils/log.h"
#include "web/web_config.h"
#include "web/web_page.h"
#include "web/web_snapshot.h"
#include "web/web_settings.h"
#include "web/web_cover_cache.h"
#include "hal/pcf85063.h"

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

static bool s_wifi_enabled = true;

static WebServer s_server(80);
static bool s_started = false;
static bool s_ready = false;
static TaskHandle_t s_web_start_task = nullptr;
static bool s_web_start_in_progress = false;
static portMUX_TYPE s_web_start_task_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_ap_mode = false;
static String s_hostname_runtime = WEBCTRL_HOSTNAME_DEFAULT;
static String s_wifi_source = "ap_fallback";

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
// 这里使用一个常驻的 1KB 缓冲区分块输出，响应大小不再决定临时堆峰值。
static constexpr size_t kWebStatusJsonChunkReserve = 1024;
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
  // 这些设置保持原来的“立即保存”语义；
  // 显示类开关 show_next_lyric / show_cover / web_cover_spin 可延迟到关机前保存。
  return old_cfg.refresh_preset != new_cfg.refresh_preset
      || old_cfg.lyric_sync_mode != new_cfg.lyric_sync_mode
      || old_cfg.wifi_enabled != new_cfg.wifi_enabled
      || old_cfg.show_wifi_info != new_cfg.show_wifi_info;
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
static String web_wifi_name_string() {
  if (s_ap_mode) return String(WEBCTRL_AP_SSID);
  if (WiFi.status() == WL_CONNECTED) {
    const String ssid = WiFi.SSID();
    if (ssid.length()) return ssid;
  }
  return String("-");
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

static bool web_load_wifi_config(std::vector<WebWifiNetwork>& nets, String& hostname) {
  hostname = WEBCTRL_HOSTNAME_DEFAULT;
  StorageSdLockGuard guard(1200);
  if (!guard) { LOGW("[网页] 跳过 WiFi 配置读取：获取 SD 锁失败"); return false; }
  File32 f = sd.open(WEBCTRL_WIFI_CONFIG_PATH, O_RDONLY);
  if (!f) { LOGW("[网页] 未找到 WiFi 配置：%s", WEBCTRL_WIFI_CONFIG_PATH); return false; }

  WebWifiNetwork cur{}; bool in_network = false; bool any = false;
  while (f.available()) {
    String line = f.readStringUntil('\n'); line.trim();
    if (line.isEmpty() || line.startsWith("#") || line.startsWith(";")) continue;
    if (line.startsWith("[")) {
      if (in_network && cur.ssid.length()) { nets.push_back(cur); any = true; }
      cur = WebWifiNetwork{};
      in_network = line.equalsIgnoreCase("[network]");
      continue;
    }
    int eq = line.indexOf('=');
    if (eq <= 0) continue;
    String key = web_trim_copy(line.substring(0, eq));
    String val = web_trim_copy(line.substring(eq + 1));
    key.toLowerCase();
    if (!in_network) {
      if (key == "hostname" && val.length()) hostname = val;
      continue;
    }
    if (key == "ssid") cur.ssid = val;
    else if (key == "password") cur.password = val;
    else if (key == "hidden") cur.hidden = web_parse_bool(val, false);
    else if (key == "channel") { long ch = val.toInt(); if (ch < 0) ch = 0; cur.channel = (int)ch; }
    else if (key == "bssid") { cur.has_bssid = web_parse_mac(val, cur.bssid); cur.bssid_text = val; }
  }
  if (in_network && cur.ssid.length()) { nets.push_back(cur); any = true; }
  f.close();
  LOGD("[网页] WiFi 配置已读取：网络数量=%d 主机名=%s", (int)nets.size(), hostname.c_str());
  return any;
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
      s_ap_mode = false; s_wifi_source = "config_file"; s_hostname_runtime = hostname;
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
    s_wifi_source = "existing_sta";
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
  s_ap_mode = true; s_wifi_source = "ap_fallback"; s_hostname_runtime = WEBCTRL_HOSTNAME_DEFAULT;
  LOGI("[网页] AP 已就绪：SSID=%s IP=%s", WEBCTRL_AP_SSID, WiFi.softAPIP().toString().c_str());
  return true;
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
static void web_handle_favicon() { web_send_no_cache_headers(); s_server.send(404, "text/plain; charset=utf-8", "not_found"); }
static void web_handle_settings_get() {
  const WebRuntimeSettings ws = web_settings_get();
  String json; json.reserve(420);
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
  json += ",\"show_wifi_info\":"; json += (ws.show_wifi_info ? "true" : "false");
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
  ws.show_wifi_info = web_parse_bool(s_server.arg("show_wifi_info"), ws.show_wifi_info);

  const bool need_immediate_save = web_settings_persistent_core_changed(old_ws, ws);
  web_settings_set(ws);

  if (need_immediate_save) {
    if (!web_settings_save_if_dirty()) { web_send_json_err("保存设置失败", 500); return; }
    web_send_json_ok_simple("settings_saved");
    return;
  }

  // 只修改封面旋转 / 网页封面 / 下一句歌词这类显示开关时，
  // 立即生效，但不立刻写 NVS；关机前由 app_power 统一保存。
  web_send_json_ok_simple(web_settings_is_dirty() ? "settings_deferred" : "settings_unchanged");
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
  snap.net_mode = web_net_mode_cstr();
  snap.ip = web_ip_string();
  snap.wifi_name = web_wifi_name_string();
  snap.hostname = s_hostname_runtime;
  snap.wifi_source = s_wifi_source;

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
  const WebUiControlSnapshot ui_control =
      web_ui_control_snapshot_get();

  json.field_uint("volume", snap.volume);
  json.field_bool("volume_locked", ui_control.volume_locked);

  json.field_string("mode", snap.mode);
  json.field_string("mode_label", snap.mode_label);
  json.field_string("view", snap.view);
  json.field_string("view_label", snap.view_label);

  json.field_int("display_pos", snap.display_pos);
  json.field_int("display_total", snap.display_total);
  json.field_int("current_group_idx", snap.current_group_idx);

  json.field_string("net_mode", snap.net_mode);
  json.field_string("ip", snap.ip);
  json.field_string("wifi_name", snap.wifi_name);
  json.field_string("hostname", snap.hostname);
  json.field_string("wifi_source", snap.wifi_source);

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
  json.field_bool("show_wifi_info", ws.show_wifi_info);

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

  json.field_bool("net_track_active", snap.net_track_active);
  json.field_int("net_track_idx", snap.net_track_idx);
  json.field_string("net_track_title", snap.net_track_title);
  json.field_string("net_track_url", snap.net_track_url);
  json.field_string("net_track_format", snap.net_track_format);
  json.field_string("net_track_artist", snap.net_track_artist);
  json.field_string("net_track_album", snap.net_track_album);
  json.field_uint("net_track_duration_ms", snap.net_track_duration_ms);
  json.field_string("net_track_state", snap.net_track_state);
  json.field_string("net_track_error", snap.net_track_error);

  if (!json.finish()) {
    web_abort_client();
  }
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

    uint32_t cover_offset = 0;
    uint32_t cover_size = 0;
    String cover_rev;
    if (!net_music_embedded_cover_get_current(req_idx,
                                              source.net_track_url,
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
                                            COVER_MP3_APIC,
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

  json += ",\"base\":\"";
  json += web_json_escape(net_music_catalog_base_url());
  json += "\"";

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
    if (mode == "strict") {
      requested_mode = AppRescanMode::StrictIncremental;
    } else if (mode == "full") {
      requested_mode = AppRescanMode::Full;
    } else if (mode != "fast" && mode != "incremental") {
      web_send_json_err("重扫模式无效：fast/strict/full");
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
                 : "rescan_fast_started"));
}

static void web_handle_wifiinfo_toggle() {
  WebRuntimeSettings ws = web_settings_get();
  ws.show_wifi_info = !ws.show_wifi_info;
  web_settings_set(ws);
  
  String json; json.reserve(80);
  json += "{\"ok\":true";
  json += ",\"show_wifi_info\":"; json += (ws.show_wifi_info ? "true" : "false");
  json += "}";
  web_send_no_cache_headers();
  s_server.send(200, "application/json; charset=utf-8", json);
}

static void web_setup_routes() {
  s_server.on("/", HTTP_GET, web_handle_root);
  s_server.on("/artists", HTTP_GET, web_handle_artists_page);
  s_server.on("/albums", HTTP_GET, web_handle_albums_page);
  s_server.on("/nfc", HTTP_GET, web_handle_nfc_page);
  s_server.on("/radios", HTTP_GET, web_handle_radios_page);
  s_server.on("/netmusic", HTTP_GET, web_handle_netmusic_page);
  s_server.on("/settings", HTTP_GET, web_handle_settings_page);
  s_server.on("/favicon.ico", HTTP_GET, web_handle_favicon);
  s_server.on("/api/status", HTTP_GET, web_handle_status);
  s_server.on("/api/artists", HTTP_GET, web_handle_artists);
  s_server.on("/api/albums", HTTP_GET, web_handle_albums);
  s_server.on("/api/artist/search_song", HTTP_GET, web_handle_artist_song_search);
  s_server.on("/api/album/search_song", HTTP_GET, web_handle_album_song_search);
  s_server.on("/api/radios", HTTP_GET, web_handle_radios);
  s_server.on("/api/netmusic", HTTP_GET, web_handle_netmusic);
  s_server.on("/api/netmusic/search", HTTP_GET, web_handle_netmusic_search);
  s_server.on("/api/artist/detail", HTTP_GET, web_handle_artist_detail);
  s_server.on("/api/album/detail", HTTP_GET, web_handle_album_detail);
  s_server.on("/api/settings", HTTP_GET, web_handle_settings_get);
  s_server.on("/api/settings", HTTP_POST, web_handle_settings_post);
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
  s_server.on("/api/next", HTTP_POST, web_handle_next);
  s_server.on("/api/prev", HTTP_POST, web_handle_prev);
  s_server.on("/api/mode/toggle", HTTP_POST, web_handle_mode_toggle);
  s_server.on("/api/mode/category", HTTP_POST, web_handle_mode_category);
  s_server.on("/api/view/toggle", HTTP_POST, web_handle_view_toggle);
  s_server.on("/api/volume", HTTP_POST, web_handle_volume);
  s_server.on("/api/ui/volume_lock", HTTP_POST, web_handle_volume_lock);
  s_server.on("/api/state/save", HTTP_POST, web_handle_state_save);
  s_server.on("/api/scan", HTTP_POST, web_handle_scan);
  s_server.on("/api/wifiinfo/toggle", HTTP_POST, web_handle_wifiinfo_toggle);
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
      s_wifi_source = "config_switch";
      s_hostname_runtime = hostname;

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
