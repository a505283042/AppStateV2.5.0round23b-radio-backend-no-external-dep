#include "web/web_settings.h"

#include <Preferences.h>
#include "utils/log.h"

static WebRuntimeSettings s_cfg{};
static bool s_dirty = false;
static const char* kPrefsNs = "webctrl";

static bool web_settings_equal(const WebRuntimeSettings& a, const WebRuntimeSettings& b)
{
  return a.refresh_preset == b.refresh_preset
      && a.lyric_sync_mode == b.lyric_sync_mode
      && a.show_next_lyric == b.show_next_lyric
      && a.show_cover == b.show_cover
      && a.web_cover_spin == b.web_cover_spin
      && a.wifi_enabled == b.wifi_enabled
      && a.show_wifi_info == b.show_wifi_info
      && a.hall_control_enabled == b.hall_control_enabled
      && a.solenoid_enabled == b.solenoid_enabled
      && a.status_led_enabled == b.status_led_enabled
      && a.status_led_brightness == b.status_led_brightness;
}

const WebRuntimeSettings& web_settings_get() { return s_cfg; }

void web_settings_set(const WebRuntimeSettings& s)
{
  if (web_settings_equal(s_cfg, s)) {
    return;
  }

  s_cfg = s;
  s_dirty = true;
}

bool web_settings_is_dirty()
{
  return s_dirty;
}

bool web_settings_save_if_dirty()
{
  if (!s_dirty) {
    LOGD("[网页] 设置没有变化，跳过 NVS 保存");
    return true;
  }

  return web_settings_save();
}

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

const char* status_led_brightness_key(StatusLedBrightness value) {
  switch (value) {
    case StatusLedBrightness::Low:    return "low";
    case StatusLedBrightness::High:   return "high";
    case StatusLedBrightness::Medium:
    default:                          return "medium";
  }
}

const char* status_led_brightness_label(StatusLedBrightness value) {
  switch (value) {
    case StatusLedBrightness::Low:    return "低";
    case StatusLedBrightness::High:   return "高";
    case StatusLedBrightness::Medium:
    default:                          return "中";
  }
}

uint8_t status_led_brightness_value(StatusLedBrightness value) {
  switch (value) {
    case StatusLedBrightness::Low:    return 24;
    case StatusLedBrightness::High:   return 64;
    case StatusLedBrightness::Medium:
    default:                          return 40;
  }
}

bool web_settings_load() {
  s_cfg = WebRuntimeSettings{};

  Preferences pref;
  if (!pref.begin(kPrefsNs, true)) {
    LOGW("[网页] 设置tings 加载 失败: 打开 NVS namespace");
    LOGD("[网页] 设置tings use 默认s");
    s_dirty = false;
    return false;
  }

  s_cfg.refresh_preset = (WebRefreshPreset)pref.getUChar("refresh", (uint8_t)s_cfg.refresh_preset);
  s_cfg.lyric_sync_mode = (WebLyricSyncMode)pref.getUChar("lyric", (uint8_t)s_cfg.lyric_sync_mode);
  s_cfg.show_next_lyric = pref.getBool("show_next", s_cfg.show_next_lyric);
  s_cfg.show_cover = pref.getBool("show_cover", s_cfg.show_cover);
  s_cfg.web_cover_spin = pref.getBool("cover_spin", s_cfg.web_cover_spin);
  s_cfg.wifi_enabled = pref.getBool("wifi_en", s_cfg.wifi_enabled);
  s_cfg.show_wifi_info = pref.getBool("wifi_info", s_cfg.show_wifi_info);
  s_cfg.hall_control_enabled = pref.getBool("hall_en", s_cfg.hall_control_enabled);
  s_cfg.solenoid_enabled = pref.getBool("sol_en", s_cfg.solenoid_enabled);
  s_cfg.status_led_enabled = pref.getBool("led_en", s_cfg.status_led_enabled);
  const uint8_t led_bri = pref.getUChar("led_bri", static_cast<uint8_t>(s_cfg.status_led_brightness));
  s_cfg.status_led_brightness = led_bri <= static_cast<uint8_t>(StatusLedBrightness::High)
      ? static_cast<StatusLedBrightness>(led_bri)
      : StatusLedBrightness::Medium;
  pref.end();
  s_dirty = false;

  LOGD("[网页] 设置已从 NVS 读取：刷新=%s 歌词=%s WiFi=%d HALL=%d SOL=%d LED=%d/%s",
       web_refresh_preset_key(s_cfg.refresh_preset),
       web_lyric_sync_mode_key(s_cfg.lyric_sync_mode),
       (int)s_cfg.wifi_enabled,
       (int)s_cfg.hall_control_enabled,
       (int)s_cfg.solenoid_enabled,
       (int)s_cfg.status_led_enabled,
       status_led_brightness_key(s_cfg.status_led_brightness));
  return true;
}

bool web_settings_save() {
  Preferences pref;
  if (!pref.begin(kPrefsNs, false)) {
    LOGE("[网页] 设置tings 保存 失败: 打开 NVS namespace");
    return false;
  }

  const bool ok = pref.putUChar("refresh", (uint8_t)s_cfg.refresh_preset)
               && pref.putUChar("lyric", (uint8_t)s_cfg.lyric_sync_mode)
               && pref.putBool("show_next", s_cfg.show_next_lyric)
               && pref.putBool("show_cover", s_cfg.show_cover)
               && pref.putBool("cover_spin", s_cfg.web_cover_spin)
               && pref.putBool("wifi_en", s_cfg.wifi_enabled)
               && pref.putBool("wifi_info", s_cfg.show_wifi_info)
               && pref.putBool("hall_en", s_cfg.hall_control_enabled)
               && pref.putBool("sol_en", s_cfg.solenoid_enabled)
               && pref.putBool("led_en", s_cfg.status_led_enabled)
               && pref.putUChar("led_bri", static_cast<uint8_t>(s_cfg.status_led_brightness));
  pref.end();

  if (!ok) {
    LOGE("[网页] 设置tings 保存 失败: 写入 NVS");
    return false;
  }

  s_dirty = false;

  LOGI("[网页] 设置已保存到 NVS：刷新=%s 歌词=%s WiFi=%d HALL=%d SOL=%d LED=%d/%s",
       web_refresh_preset_key(s_cfg.refresh_preset),
       web_lyric_sync_mode_key(s_cfg.lyric_sync_mode),
       (int)s_cfg.wifi_enabled,
       (int)s_cfg.hall_control_enabled,
       (int)s_cfg.solenoid_enabled,
       (int)s_cfg.status_led_enabled,
       status_led_brightness_key(s_cfg.status_led_brightness));
  return true;
}
