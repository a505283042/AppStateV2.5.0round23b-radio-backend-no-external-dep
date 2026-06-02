#include "web/web_settings.h"

#include <Preferences.h>
#include "utils/log.h"

static WebRuntimeSettings s_cfg{};
static const char* kPrefsNs = "webctrl";



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
  if (!pref.begin(kPrefsNs, true)) {
    LOGW("[WEB] settings load failed: open NVS namespace");
    LOGI("[WEB] settings use defaults");
    return false;
  }

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

bool web_settings_save() {
  Preferences pref;
  if (!pref.begin(kPrefsNs, false)) {
    LOGE("[WEB] settings save failed: open NVS namespace");
    return false;
  }

  const bool ok = pref.putUChar("refresh", (uint8_t)s_cfg.refresh_preset)
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
