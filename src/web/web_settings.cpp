#include "web/web_settings.h"

#include <Preferences.h>
#include <SdFat.h>

#include "storage/storage_io.h"
#include "utils/log.h"
#include "web/web_config.h"

extern SdFat sd;

static WebRuntimeSettings s_cfg{};
static const char* kPrefsNs = "webctrl";
static const char* kPrefsInit = "init";

static String trim_copy(const String& in) {
  String s = in;
  s.trim();
  return s;
}

static bool parse_bool(const String& v, bool defv) {
  String s = trim_copy(v);
  s.toLowerCase();
  if (s == "1" || s == "true" || s == "yes" || s == "on") return true;
  if (s == "0" || s == "false" || s == "no" || s == "off") return false;
  return defv;
}

static WebRefreshPreset parse_refresh_preset(const String& v, WebRefreshPreset defv) {
  String s = trim_copy(v);
  s.toLowerCase();
  if (s == "power" || s == "power_save") return WebRefreshPreset::POWER_SAVE;
  if (s == "balanced") return WebRefreshPreset::BALANCED;
  if (s == "smooth") return WebRefreshPreset::SMOOTH;
  return defv;
}

static WebLyricSyncMode parse_lyric_sync_mode(const String& v, WebLyricSyncMode defv) {
  String s = trim_copy(v);
  s.toLowerCase();
  if (s == "precise") return WebLyricSyncMode::PRECISE;
  if (s == "balanced") return WebLyricSyncMode::BALANCED;
  if (s == "follow_poll" || s == "wait_poll") return WebLyricSyncMode::FOLLOW_POLL;
  return defv;
}

static bool load_legacy_sd_settings(WebRuntimeSettings& out) {
  StorageSdLockGuard guard(1000);
  if (!guard) {
    LOGW("[WEB] legacy settings load skip: SD lock failed");
    return false;
  }

  File32 f = sd.open(WEBCTRL_SETTINGS_CONFIG_PATH, O_RDONLY);
  if (!f) {
    LOGI("[WEB] settings file not found, use defaults/NVS: %s", WEBCTRL_SETTINGS_CONFIG_PATH);
    return false;
  }

  String line;
  while (f.available()) {
    line = f.readStringUntil('\n');
    line.trim();
    if (line.isEmpty() || line.startsWith("#") || line.startsWith(";")) continue;
    const int eq = line.indexOf('=');
    if (eq <= 0) continue;
    String key = trim_copy(line.substring(0, eq));
    String val = trim_copy(line.substring(eq + 1));
    key.toLowerCase();

    if (key == "refresh_preset") out.refresh_preset = parse_refresh_preset(val, out.refresh_preset);
    else if (key == "lyric_sync_mode") out.lyric_sync_mode = parse_lyric_sync_mode(val, out.lyric_sync_mode);
    else if (key == "show_next_lyric") out.show_next_lyric = parse_bool(val, out.show_next_lyric);
    else if (key == "show_cover") out.show_cover = parse_bool(val, out.show_cover);
    else if (key == "web_cover_spin") out.web_cover_spin = parse_bool(val, out.web_cover_spin);
    // 兼容旧 round9 配置：
    else if (key == "poll_ms") {
      const long x = val.toInt();
      if (x > 0) {
        if (x >= 1200) out.refresh_preset = WebRefreshPreset::POWER_SAVE;
        else if (x <= 750) out.refresh_preset = WebRefreshPreset::SMOOTH;
        else out.refresh_preset = WebRefreshPreset::BALANCED;
      }
    } else if (key == "lyric_wait_poll_threshold_ms") {
      const long x = val.toInt();
      if (x <= 100) out.lyric_sync_mode = WebLyricSyncMode::PRECISE;
      else if (x >= 240) out.lyric_sync_mode = WebLyricSyncMode::FOLLOW_POLL;
      else out.lyric_sync_mode = WebLyricSyncMode::BALANCED;
    }
  }
  f.close();
  LOGI("[WEB] legacy SD settings imported: %s", WEBCTRL_SETTINGS_CONFIG_PATH);
  return true;
}

const WebRuntimeSettings& web_settings_get() { return s_cfg; }
void web_settings_set(const WebRuntimeSettings& s) { s_cfg = s; }

const char* web_refresh_preset_key(WebRefreshPreset p) {
  switch (p) {
    case WebRefreshPreset::POWER_SAVE: return "power";
    case WebRefreshPreset::BALANCED:   return "balanced";
    case WebRefreshPreset::SMOOTH:     return "smooth";
    default:                           return "balanced";
  }
}

const char* web_refresh_preset_label(WebRefreshPreset p) {
  switch (p) {
    case WebRefreshPreset::POWER_SAVE: return "省流量 / 省电";
    case WebRefreshPreset::BALANCED:   return "平衡";
    case WebRefreshPreset::SMOOTH:     return "流畅";
    default:                           return "平衡";
  }
}

uint32_t web_refresh_preset_poll_ms(WebRefreshPreset p) {
  switch (p) {
    case WebRefreshPreset::POWER_SAVE: return 1400;
    case WebRefreshPreset::BALANCED:   return 1000;
    case WebRefreshPreset::SMOOTH:     return 650;
    default:                           return 1000;
  }
}

const char* web_lyric_sync_mode_key(WebLyricSyncMode m) {
  switch (m) {
    case WebLyricSyncMode::PRECISE:     return "precise";
    case WebLyricSyncMode::BALANCED:    return "balanced";
    case WebLyricSyncMode::FOLLOW_POLL: return "follow_poll";
    default:                            return "balanced";
  }
}

const char* web_lyric_sync_mode_label(WebLyricSyncMode m) {
  switch (m) {
    case WebLyricSyncMode::PRECISE:     return "精准优先";
    case WebLyricSyncMode::BALANCED:    return "平衡";
    case WebLyricSyncMode::FOLLOW_POLL: return "等轮询优先";
    default:                            return "平衡";
  }
}

uint32_t web_lyric_sync_mode_threshold_ms(WebLyricSyncMode m) {
  switch (m) {
    case WebLyricSyncMode::PRECISE:     return 80;
    case WebLyricSyncMode::BALANCED:    return 150;
    case WebLyricSyncMode::FOLLOW_POLL: return 280;
    default:                            return 150;
  }
}

bool web_settings_load() {
  s_cfg = WebRuntimeSettings{};

  Preferences pref;
  if (pref.begin(kPrefsNs, true)) {
    const bool inited = pref.getBool(kPrefsInit, false);
    if (inited) {
      s_cfg.refresh_preset = (WebRefreshPreset)pref.getUChar("refresh", (uint8_t)s_cfg.refresh_preset);
      s_cfg.lyric_sync_mode = (WebLyricSyncMode)pref.getUChar("lyric", (uint8_t)s_cfg.lyric_sync_mode);
      s_cfg.show_next_lyric = pref.getBool("show_next", s_cfg.show_next_lyric);
      s_cfg.show_cover = pref.getBool("show_cover", s_cfg.show_cover);
      s_cfg.web_cover_spin = pref.getBool("cover_spin", s_cfg.web_cover_spin);
      pref.end();
      LOGI("[WEB] settings loaded from NVS: refresh=%s lyric=%s show_next=%d show_cover=%d cover_spin=%d",
           web_refresh_preset_key(s_cfg.refresh_preset),
           web_lyric_sync_mode_key(s_cfg.lyric_sync_mode),
           (int)s_cfg.show_next_lyric,
           (int)s_cfg.show_cover,
           (int)s_cfg.web_cover_spin);
      return true;
    }
    pref.end();
  }

  if (load_legacy_sd_settings(s_cfg)) {
    // 只要导入成功，就顺手写入 NVS，后续不再依赖 SD 保存网页设置。
    web_settings_save();
    return true;
  }

  LOGI("[WEB] settings use defaults");
  return false;
}

bool web_settings_save() {
  Preferences pref;
  if (!pref.begin(kPrefsNs, false)) {
    LOGE("[WEB] settings save failed: open NVS namespace");
    return false;
  }

  const bool ok = pref.putBool(kPrefsInit, true)
               && pref.putUChar("refresh", (uint8_t)s_cfg.refresh_preset)
               && pref.putUChar("lyric", (uint8_t)s_cfg.lyric_sync_mode)
               && pref.putBool("show_next", s_cfg.show_next_lyric)
               && pref.putBool("show_cover", s_cfg.show_cover)
               && pref.putBool("cover_spin", s_cfg.web_cover_spin);
  pref.end();

  if (!ok) {
    LOGE("[WEB] settings save failed: write NVS");
    return false;
  }

  LOGI("[WEB] settings saved to NVS: refresh=%s lyric=%s show_next=%d show_cover=%d cover_spin=%d",
       web_refresh_preset_key(s_cfg.refresh_preset),
       web_lyric_sync_mode_key(s_cfg.lyric_sync_mode),
       (int)s_cfg.show_next_lyric,
       (int)s_cfg.show_cover,
       (int)s_cfg.web_cover_spin);
  return true;
}
