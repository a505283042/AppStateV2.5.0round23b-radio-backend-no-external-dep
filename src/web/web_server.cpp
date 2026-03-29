#include "web/web_server.h"

#include <WiFi.h>
#include <WebServer.h>
#include <SdFat.h>
#include <vector>

#include "app_state.h"
#include "app_flags.h"
#include "audio/audio_service.h"
#include "audio/audio.h"
#include "player_control.h"
#include "player_snapshot.h"
#include "player_state.h"
#include "player_source.h"
#include "radio/radio_catalog.h"
#include "player_list_select.h"
#include "player_playlist.h"
#include "storage/storage_catalog_v3.h"
#include "storage/storage_view_v3.h"
#include "storage/storage_io.h"
#include "storage/storage_groups_v3.h"
#include "ui/ui.h"
#include "utils/log.h"
#include "web/web_config.h"
#include "web/web_page.h"
#include "web/web_snapshot.h"
#include "web/web_settings.h"

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

static WebServer s_server(80);
static bool s_started = false;
static bool s_ready = false;
static bool s_ap_mode = false;
static String s_hostname_runtime = WEBCTRL_HOSTNAME_DEFAULT;
static String s_wifi_source = "ap_fallback";

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

static bool web_client_alive() {
  return s_server.client().connected();
}

static bool web_send_chunk(const char* s) {
  if (!web_client_alive()) return false;
  s_server.sendContent(s);
  return web_client_alive();
}

static bool web_send_chunk(const String& s) {
  if (!web_client_alive()) return false;
  s_server.sendContent(s);
  return web_client_alive();
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

static bool web_parse_bool(const String& v, bool defv=false) {
  String s = web_trim_copy(v); s.toLowerCase();
  if (s=="1"||s=="true"||s=="yes"||s=="on") return true;
  if (s=="0"||s=="false"||s=="no"||s=="off") return false;
  return defv;
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
  if (msg && *msg) { json += ",\"message\":\""; json += web_json_escape(msg); json += "\""; }
  json += "}";
  s_server.send(200, "application/json; charset=utf-8", json);
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
  if (g_rescanning) { web_send_json_err("正在扫描音乐库"); return false; }
  if (player_list_select_is_active()) { web_send_json_err("当前处于列表选择模式"); return false; }
  return true;
}

static bool web_load_wifi_config(std::vector<WebWifiNetwork>& nets, String& hostname) {
  hostname = WEBCTRL_HOSTNAME_DEFAULT;
  StorageSdLockGuard guard(1200);
  if (!guard) { LOGW("[WEB] wifi config load skip: SD lock failed"); return false; }
  File32 f = sd.open(WEBCTRL_WIFI_CONFIG_PATH, O_RDONLY);
  if (!f) { LOGI("[WEB] wifi config not found: %s", WEBCTRL_WIFI_CONFIG_PATH); return false; }

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
  LOGI("[WEB] wifi config loaded: %d network(s), hostname=%s", (int)nets.size(), hostname.c_str());
  return any;
}

static bool web_try_connect_one(const WebWifiNetwork& n, const String& hostname) {
  if (n.ssid.isEmpty()) return false;
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(hostname.c_str());
  WiFi.disconnect(true, true);
  delay(50);
  if (n.channel > 0 || n.has_bssid) {
    WiFi.begin(n.ssid.c_str(), n.password.c_str(), n.channel > 0 ? n.channel : 0, n.has_bssid ? n.bssid : nullptr, true);
  } else {
    WiFi.begin(n.ssid.c_str(), n.password.c_str());
  }
  LOGI("[WEB] connecting STA ssid=%s%s", n.ssid.c_str(), n.hidden ? " (hidden)" : "");
  const uint32_t t0 = millis();
  while ((millis() - t0) < WEBCTRL_STA_CONNECT_TIMEOUT_MS) {
    if (WiFi.status() == WL_CONNECTED) {
      WiFi.setSleep(false);
      LOGI("[WEB] STA connected ip=%s", WiFi.localIP().toString().c_str());
      s_ap_mode = false; s_wifi_source = "config_file"; s_hostname_runtime = hostname;
      return true;
    }
    delay(200);
  }
  LOGI("[WEB] STA connect timeout for ssid=%s", n.ssid.c_str());
  return false;
}

static bool web_try_connect_sta_from_config() {
  std::vector<WebWifiNetwork> nets;
  String hostname;
  if (!web_load_wifi_config(nets, hostname) || nets.empty()) return false;
  for (const auto& n : nets) {
    if (web_try_connect_one(n, hostname)) return true;
  }
  WiFi.disconnect(true, true);
  return false;
}

static bool web_start_ap_fallback() {
  WiFi.mode(WIFI_AP);
  WiFi.setHostname(WEBCTRL_HOSTNAME_DEFAULT);
  const bool ok = WiFi.softAP(WEBCTRL_AP_SSID, WEBCTRL_AP_PASS);
  if (!ok) { LOGE("[WEB] AP start failed"); return false; }
  WiFi.setSleep(false);
  s_ap_mode = true; s_wifi_source = "ap_fallback"; s_hostname_runtime = WEBCTRL_HOSTNAME_DEFAULT;
  LOGI("[WEB] AP ready ssid=%s ip=%s", WEBCTRL_AP_SSID, WiFi.softAPIP().toString().c_str());
  return true;
}

static uint32_t web_clamp_u32_arg(const char* name, uint32_t defv, uint32_t lo, uint32_t hi) {
  String s = s_server.arg(name); if (!s.length()) return defv; long v = s.toInt(); if (v < (long)lo) v = (long)lo; if (v > (long)hi) v = (long)hi; return (uint32_t)v;
}
static bool web_parse_int_arg(const char* name, int& out) {
  String s = s_server.arg(name);
  if (!s.length()) return false;
  out = s.toInt();
  return true;
}
static bool web_status_mode_is_artist() {
  return g_play_mode == PLAY_MODE_ARTIST_SEQ || g_play_mode == PLAY_MODE_ARTIST_RND;
}
static bool web_status_mode_is_album() {
  return g_play_mode == PLAY_MODE_ALBUM_SEQ || g_play_mode == PLAY_MODE_ALBUM_RND;
}
static bool web_radio_catalog_ensure_loaded() {
  return radio_catalog_is_loaded();
}
static void web_send_radio_list_json() {
  const bool loaded = web_radio_catalog_ensure_loaded();
  const auto& items = radio_catalog_items();

  LOGI("[WEB] radios total=%u (stream-batch)", (unsigned)items.size());

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

  LOGI("[WEB] group list type=%s total=%u (stream-batch)",
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
  head += web_json_escape(snap.mode);
  head += "\"";
  head += ",\"mode_label\":\"";
  head += web_json_escape(snap.mode_label);
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

  const int total_tracks = (int)g.track_indices.size();
  if (offset > total_tracks) offset = total_tracks;
  const int end = (offset + limit > total_tracks) ? total_tracks : (offset + limit);

  LOGI("[WEB] group detail idx=%d is_album=%d offset=%d limit=%d returned=%d total=%d",
       group_idx, is_album ? 1 : 0, offset, limit, end - offset, total_tracks);

  web_send_no_cache_headers();
  s_server.sendHeader("Connection", "close");
  s_server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  s_server.send(200, "application/json; charset=utf-8", "{");

  if (!web_send_chunk("\"ok\":true")) return;
  if (!web_send_chunk(",\"idx\":")) return;
  if (!web_send_chunk(String(group_idx))) return;

  if (!web_send_chunk(",\"name\":\"")) return;
  if (!web_send_chunk(web_json_escape(g_name))) return;
  if (!web_send_chunk("\"")) return;

  if (is_album) {
    if (!web_send_chunk(",\"primary_artist\":\"")) return;
    if (!web_send_chunk(web_json_escape(g_pa))) return;
    if (!web_send_chunk("\"")) return;
  }

  if (!web_send_chunk(",\"track_count\":")) return;
  if (!web_send_chunk(String(total_tracks))) return;

  if (!web_send_chunk(",\"offset\":")) return;
  if (!web_send_chunk(String(offset))) return;

  if (!web_send_chunk(",\"limit\":")) return;
  if (!web_send_chunk(String(limit))) return;

  if (!web_send_chunk(",\"returned\":")) return;
  if (!web_send_chunk(String(end - offset))) return;

  if (!web_send_chunk(",\"tracks\":[")) return;

  bool first = true;
  for (int i = offset; i < end; ++i) {
    int track_idx = (int)g.track_indices[(size_t)i];

    TrackViewV3 v{};
    if (!storage_catalog_v3_get_track_view((uint32_t)track_idx, v, "/Music") || !v.valid) {
      continue;
    }

    if (!first) {
      if (!web_send_chunk(",")) return;
    }
    first = false;

    if (!web_send_chunk("{\"track_idx\":")) return;
    if (!web_send_chunk(String(track_idx))) return;

    if (!web_send_chunk(",\"title\":\"")) return;
    if (!web_send_chunk(web_json_escape(v.title))) return;
    if (!web_send_chunk("\"")) return;

    if (!web_send_chunk(",\"artist\":\"")) return;
    if (!web_send_chunk(web_json_escape(v.artist))) return;
    if (!web_send_chunk("\"")) return;

    if (!web_send_chunk(",\"album\":\"")) return;
    if (!web_send_chunk(web_json_escape(v.album))) return;
    if (!web_send_chunk("\"")) return;

    const bool has_cover = (v.cover_source != COVER_NONE && (v.cover_size > 0 || v.cover_path.length() > 0));
    if (!web_send_chunk(",\"has_cover\":")) return;
    if (!web_send_chunk(has_cover ? "true" : "false")) return;

    if (has_cover) {
      if (!web_send_chunk(",\"cover_url\":\"")) return;
      if (!web_send_chunk(web_json_escape(String("/api/cover/current?track=") + String(track_idx)))) return;
      if (!web_send_chunk("\"")) return;
    }

    if (!web_send_chunk("}")) return;
  }

  if (!web_send_chunk("]}")) return;
  web_end_stream_response();
}
static bool web_play_group_impl(bool is_album, int group_idx) {
  const auto& groups = is_album ? player_playlist_album_groups() : player_playlist_artist_groups();
  if (group_idx < 0 || group_idx >= (int)groups.size()) return false;
  g_play_mode = is_album ? PLAY_MODE_ALBUM_SEQ : PLAY_MODE_ARTIST_SEQ;
  player_playlist_set_current_group_idx(group_idx);
  player_playlist_force_rebuild();
  const auto& playlist = player_playlist_get_current();
  if (playlist.empty()) return false;
  return player_play_idx_v3((uint32_t)playlist[0], true, true);
}
static void web_handle_artists_page() {
  web_send_no_cache_headers();
  s_server.send_P(200, "text/html; charset=utf-8", WEBCTRL_ARTISTS_HTML);
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
  const auto& ws = web_settings_get();
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
  json += "}";
  web_send_no_cache_headers();
  s_server.send(200, "application/json; charset=utf-8", json);
}
static void web_handle_settings_post() {
  WebRuntimeSettings ws = web_settings_get();
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
  web_settings_set(ws);
  if (!web_settings_save()) { web_send_json_err("保存设置失败", 500); return; }
  web_send_json_ok_simple("settings_saved");
}
static void web_handle_status() {
  WebPlayerSnapshot snap = web_snapshot_capture();
  snap.net_mode = web_net_mode_cstr();
  snap.ip = web_ip_string();
  snap.wifi_name = web_wifi_name_string();
  snap.hostname = s_hostname_runtime;
  snap.wifi_source = s_wifi_source;

  const auto& ws = web_settings_get();

  String json;
  json.reserve(2600);

  json += "{\"ok\":";
  json += (snap.ok ? "true" : "false");

  json += ",\"app_state\":\"" + web_json_escape(snap.app_state) + "\"";
  json += ",\"app_state_label\":\"" + web_json_escape(snap.app_state_label) + "\"";
  json += ",\"rescanning\":";
  json += (snap.rescanning ? "true" : "false");

  json += ",\"is_playing\":";
  json += (snap.is_playing ? "true" : "false");
  json += ",\"is_paused\":";
  json += (snap.is_paused ? "true" : "false");

  json += ",\"track_idx\":" + String(snap.track_idx);
  json += ",\"title\":\"" + web_json_escape(snap.title) + "\"";
  json += ",\"artist\":\"" + web_json_escape(snap.artist) + "\"";
  json += ",\"album\":\"" + web_json_escape(snap.album) + "\"";

  json += ",\"play_ms\":" + String(snap.play_ms);
  json += ",\"total_ms\":" + String(snap.total_ms);
  json += ",\"volume\":" + String(snap.volume);

  json += ",\"mode\":\"" + web_json_escape(snap.mode) + "\"";
  json += ",\"mode_label\":\"" + web_json_escape(snap.mode_label) + "\"";
  json += ",\"view\":\"" + web_json_escape(snap.view) + "\"";
  json += ",\"view_label\":\"" + web_json_escape(snap.view_label) + "\"";

  json += ",\"display_pos\":" + String(snap.display_pos);
  json += ",\"display_total\":" + String(snap.display_total);
  json += ",\"current_group_idx\":" + String(snap.current_group_idx);

  json += ",\"net_mode\":\"" + web_json_escape(snap.net_mode) + "\"";
  json += ",\"ip\":\"" + web_json_escape(snap.ip) + "\"";
  json += ",\"wifi_name\":\"" + web_json_escape(snap.wifi_name) + "\"";
  json += ",\"hostname\":\"" + web_json_escape(snap.hostname) + "\"";
  json += ",\"wifi_source\":\"" + web_json_escape(snap.wifi_source) + "\"";

  json += ",\"can_cancel_scan\":";
  json += (snap.can_cancel_scan ? "true" : "false");
  json += ",\"scan_action_label\":\"" + web_json_escape(snap.scan_action_label) + "\"";

  json += ",\"has_lyrics\":";
  json += (snap.has_lyrics ? "true" : "false");
  json += ",\"current_lyric\":\"" + web_json_escape(snap.current_lyric) + "\"";
  json += ",\"next_lyric\":\"" + web_json_escape(snap.next_lyric) + "\"";
  json += ",\"following_lyric\":\"" + web_json_escape(snap.following_lyric) + "\"";

  json += ",\"show_next_lyric\":";
  json += (snap.show_next_lyric ? "true" : "false");
  json += ",\"show_cover\":";
  json += (snap.show_cover ? "true" : "false");
  json += ",\"web_cover_spin\":";
  json += (snap.web_cover_spin ? "true" : "false");

  json += ",\"lyric_sync_mode\":\"";
  json += web_lyric_sync_mode_key(ws.lyric_sync_mode);
  json += "\"";

  json += ",\"lyric_sync_mode_label\":\"";
  json += web_lyric_sync_mode_label(ws.lyric_sync_mode);
  json += "\"";

  json += ",\"lyric_wait_poll_threshold_ms\":";
  json += String((int)web_lyric_sync_mode_threshold_ms(ws.lyric_sync_mode));

  json += ",\"current_lyric_start_ms\":" + String(snap.current_lyric_start_ms);
  json += ",\"next_lyric_start_ms\":" + String(snap.next_lyric_start_ms);
  json += ",\"following_lyric_start_ms\":" + String(snap.following_lyric_start_ms);
  json += ",\"next_poll_ms\":" + String(snap.next_poll_ms);

  const bool is_radio_cover = (snap.source_type == "radio");
  const bool allow_cover_fetch_now =
      snap.has_cover &&
      (is_radio_cover || !snap.is_playing || snap.play_ms >= 600);

  json += ",\"has_cover\":";
  json += (allow_cover_fetch_now ? "true" : "false");

  json += ",\"cover_url\":\"";
  json += web_json_escape(allow_cover_fetch_now ? snap.cover_url : String(""));
  json += "\"";

  json += ",\"source_type\":\"" + web_json_escape(snap.source_type) + "\"";

  json += ",\"radio_active\":";
  json += (snap.radio_active ? "true" : "false");
  json += ",\"radio_idx\":" + String(snap.radio_idx);
  json += ",\"radio_name\":\"" + web_json_escape(snap.radio_name) + "\"";
  json += ",\"radio_format\":\"" + web_json_escape(snap.radio_format) + "\"";
  json += ",\"radio_region\":\"" + web_json_escape(snap.radio_region) + "\"";
  json += ",\"radio_state\":\"" + web_json_escape(snap.radio_state) + "\"";
  json += ",\"radio_error\":\"" + web_json_escape(snap.radio_error) + "\"";
  json += ",\"radio_stream_title\":\"" + web_json_escape(snap.radio_stream_title) + "\"";
  json += ",\"radio_backend\":\"" + web_json_escape(snap.radio_backend) + "\"";
  json += ",\"radio_bitrate\":" + String(snap.radio_bitrate);

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
  bool is_remote = false;
  String logo = web_get_current_radio_logo(&is_remote);
  if (!logo.length()) {
    web_send_json_err("当前电台没有封面", 404);
    return;
  }

  if (is_remote) {
    s_server.sendHeader("Location", logo, true);
    s_server.send(302, "text/plain; charset=utf-8", "");
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
  web_send_no_cache_headers();
  client.print("HTTP/1.1 200 OK\r\n");
  client.print(is_png ? "Content-Type: image/png\r\n" : "Content-Type: image/jpeg\r\n");
  client.printf("Content-Length: %u\r\n", (unsigned)len);
  client.printf("ETag: \"cover-radio-%d\"\r\n", player_source_get().radio_idx);
  client.print("Connection: close\r\n\r\n");
  client.write(buf, len);

  free(buf);
}

static void web_handle_cover_current() {
  int cur = player_state_current_index();
  int req_track = -1;
  if (web_parse_int_arg("track", req_track)) cur = req_track;
  if (cur < 0) { web_send_json_err("当前没有曲目", 404); return; }
  TrackViewV3 v{};
  if (!storage_catalog_v3_get_track_view((uint32_t)cur, v, "/Music") || !v.valid) { web_send_json_err("读取曲目信息失败", 404); return; }
  if (v.cover_source == COVER_NONE || (v.cover_size == 0 && v.cover_path.length() == 0)) { web_send_json_err("当前曲目没有封面", 404); return; }
  uint8_t* buf = nullptr; size_t len = 0; bool is_png = false;
  const bool ok = audio_service_fetch_cover((CoverSource)v.cover_source, v.audio_path.c_str(), v.cover_path.c_str(), v.cover_offset, v.cover_size, &buf, &len, &is_png, true);
  if (!ok || !buf || len == 0) { if (buf) free(buf); web_send_json_err("封面读取失败", 500); return; }
  WiFiClient client = s_server.client();
  client.print("HTTP/1.1 200 OK\r\n");
  client.print(is_png ? "Content-Type: image/png\r\n" : "Content-Type: image/jpeg\r\n");
  client.printf("Content-Length: %u\r\n", (unsigned)len);
  client.printf("Cache-Control: public, max-age=86400, immutable\r\n");
  client.printf("ETag: \"cover-track-%d\"\r\n", cur);
  client.print("Connection: close\r\n");
  client.print("\r\n");
  client.write(buf, len);
  client.flush();
  free(buf);
}
static void web_handle_artists() {
  web_send_group_list_json(player_playlist_artist_groups(), false);
}
static void web_handle_albums() {
  web_send_group_list_json(player_playlist_album_groups(), true);
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
  int track_idx = -1; if (!web_parse_int_arg("idx", track_idx)) { web_send_json_err("缺少 idx 参数"); return; }
  if (track_idx < 0 || track_idx >= (int)storage_catalog_v3_track_count()) { web_send_json_err("曲目不存在", 404); return; }
  String mode = s_server.arg("mode"); mode.toLowerCase();
  int group_idx = -1; web_parse_int_arg("group_idx", group_idx);
  if (mode == "artist") {
    g_play_mode = PLAY_MODE_ARTIST_SEQ;
    if (group_idx >= 0) player_playlist_set_current_group_idx(group_idx);
    else (void)player_playlist_align_group_context_for_track(track_idx, false);
  } else if (mode == "album") {
    g_play_mode = PLAY_MODE_ALBUM_SEQ;
    if (group_idx >= 0) player_playlist_set_current_group_idx(group_idx);
    else (void)player_playlist_align_group_context_for_track(track_idx, false);
  } else {
    g_play_mode = PLAY_MODE_ALL_SEQ;
    player_playlist_set_current_group_idx(-1);
  }
  player_playlist_force_rebuild();
  if (!player_play_idx_v3((uint32_t)track_idx, true, true)) { web_send_json_err("曲目播放失败", 500); return; }
  web_send_json_ok_simple("track_play_started");
}
static void web_handle_radios_page() {
  web_send_no_cache_headers();
  s_server.send_P(200, "text/html; charset=utf-8", WEBCTRL_RADIOS_HTML);
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
static void web_handle_playpause() { if (!web_require_player_state()) return; player_toggle_play(); web_send_json_ok_simple(); }
static void web_handle_next() { if (!web_require_player_state()) return; player_next_track(); web_send_json_ok_simple(); }
static void web_handle_prev() { if (!web_require_player_state()) return; player_prev_track(); web_send_json_ok_simple(); }
static void web_handle_mode_toggle() { if (!web_require_player_state()) return; player_toggle_random(); web_send_json_ok_simple(); }
static void web_handle_mode_category() { if (!web_require_player_state()) return; player_cycle_mode_category(); web_send_json_ok_simple(); }
static void web_handle_view_toggle() { if (!web_require_player_state()) return; ui_toggle_view(); web_send_json_ok_simple(); }
static bool web_parse_volume_arg(uint8_t& out_value) { String s = s_server.arg("value"); if (s.length()==0) s = s_server.arg("v"); if (s.length()==0) return false; int v=s.toInt(); if (v<0) v=0; if (v>100) v=100; out_value=(uint8_t)v; return true; }
static void web_handle_volume() {
  if (g_app_state != STATE_PLAYER) { web_send_json_err("当前不在播放器状态"); return; }
  uint8_t v = 0; if (!web_parse_volume_arg(v)) { web_send_json_err("缺少音量参数 value"); return; }
  audio_set_volume(v); ui_set_volume(v); ui_volume_key_pressed(); web_send_json_ok_simple();
}
static void web_handle_state_save() {
  if (!web_require_player_state()) return;
  if (!player_snapshot_save_to_nvs()) { web_send_json_err("保存当前状态失败", 500); return; }
  web_send_json_ok_simple("player_state_saved");
}
static void web_handle_scan() {
  if (g_rescanning) {
    if (!app_request_cancel_rescan()) { web_send_json_err("当前没有正在进行的重扫"); return; }
    web_send_json_ok_simple(g_abort_scan ? "rescan_cancel_requested" : "rescan_cancel_pending"); return;
  }
  if (!app_request_start_rescan()) { web_send_json_err("当前状态不允许开始重扫"); return; }
  web_send_json_ok_simple("rescan_started");
}
static void web_setup_routes() {
  s_server.on("/", HTTP_GET, web_handle_root);
  s_server.on("/artists", HTTP_GET, web_handle_artists_page);
  s_server.on("/albums", HTTP_GET, web_handle_albums_page);
  s_server.on("/radios", HTTP_GET, web_handle_radios_page);
  s_server.on("/settings", HTTP_GET, web_handle_settings_page);
  s_server.on("/favicon.ico", HTTP_GET, web_handle_favicon);
  s_server.on("/api/status", HTTP_GET, web_handle_status);
  s_server.on("/api/artists", HTTP_GET, web_handle_artists);
  s_server.on("/api/albums", HTTP_GET, web_handle_albums);
  s_server.on("/api/radios", HTTP_GET, web_handle_radios);
  s_server.on("/api/artist/detail", HTTP_GET, web_handle_artist_detail);
  s_server.on("/api/album/detail", HTTP_GET, web_handle_album_detail);
  s_server.on("/api/settings", HTTP_GET, web_handle_settings_get);
  s_server.on("/api/settings", HTTP_POST, web_handle_settings_post);
  s_server.on("/api/cover/current", HTTP_GET, web_handle_cover_current);
  s_server.on("/api/radio/logo/current", HTTP_GET, web_handle_radio_logo_current);
  s_server.on("/api/artist/play", HTTP_POST, web_handle_artist_play);
  s_server.on("/api/album/play", HTTP_POST, web_handle_album_play);
  s_server.on("/api/track/play", HTTP_POST, web_handle_track_play);
  s_server.on("/api/radio/play", HTTP_POST, web_handle_radio_play);
  s_server.on("/api/radio/stop", HTTP_POST, web_handle_radio_stop);
  s_server.on("/api/playpause", HTTP_POST, web_handle_playpause);
  s_server.on("/api/next", HTTP_POST, web_handle_next);
  s_server.on("/api/prev", HTTP_POST, web_handle_prev);
  s_server.on("/api/mode/toggle", HTTP_POST, web_handle_mode_toggle);
  s_server.on("/api/mode/category", HTTP_POST, web_handle_mode_category);
  s_server.on("/api/view/toggle", HTTP_POST, web_handle_view_toggle);
  s_server.on("/api/volume", HTTP_POST, web_handle_volume);
  s_server.on("/api/state/save", HTTP_POST, web_handle_state_save);
  s_server.on("/api/scan", HTTP_POST, web_handle_scan);
  s_server.onNotFound([](){ web_send_json_err("not_found", 404); });
}
void web_server_start() {
#if WEBCTRL_ENABLED
  if (s_started) return;
  s_started = true;
  WiFi.persistent(false); WiFi.setAutoReconnect(true);
  web_settings_load();
  const bool net_ok = web_try_connect_sta_from_config() || web_start_ap_fallback();
  if (!net_ok) { LOGE("[WEB] network start failed, web disabled"); s_ready = false; return; }
  web_setup_routes(); s_server.begin(); s_ready = true; LOGI("[WEB] server started: http://%s/", web_ip_string().c_str());
#else
  s_started = true; s_ready = false;
#endif
}
void web_server_poll() {
#if WEBCTRL_ENABLED
  if (!s_ready) return;
  s_server.handleClient();
#endif
}
bool web_server_started() { return s_started; }
bool web_server_ready() { return s_ready; }
