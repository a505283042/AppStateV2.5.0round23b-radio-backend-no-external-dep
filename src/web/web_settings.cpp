#include "web/web_settings.h"

#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

#include "utils/log.h"

static WebRuntimeSettings s_cfg{};
static bool s_dirty = false;
static uint32_t s_revision = 0;
static portMUX_TYPE s_settings_mux = portMUX_INITIALIZER_UNLOCKED;
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

WebRuntimeSettings web_settings_get()
{
  portENTER_CRITICAL(&s_settings_mux);
  const WebRuntimeSettings snapshot = s_cfg;
  portEXIT_CRITICAL(&s_settings_mux);
  return snapshot;
}

void web_settings_set(const WebRuntimeSettings& s)
{
  portENTER_CRITICAL(&s_settings_mux);
  if (!web_settings_equal(s_cfg, s)) {
    s_cfg = s;
    s_dirty = true;
    ++s_revision;
  }
  portEXIT_CRITICAL(&s_settings_mux);
}

bool web_settings_is_dirty()
{
  portENTER_CRITICAL(&s_settings_mux);
  const bool dirty = s_dirty;
  portEXIT_CRITICAL(&s_settings_mux);
  return dirty;
}

bool web_settings_save_if_dirty()
{
  if (!web_settings_is_dirty()) {
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
  WebRuntimeSettings loaded{};

  Preferences pref;
  if (!pref.begin(kPrefsNs, true)) {
    LOGW("[网页] 设置加载失败：无法打开 NVS namespace");
    LOGD("[网页] 使用默认设置");

    portENTER_CRITICAL(&s_settings_mux);
    s_cfg = loaded;
    s_dirty = false;
    ++s_revision;
    portEXIT_CRITICAL(&s_settings_mux);
    return false;
  }

  const uint8_t refresh = pref.getUChar("refresh", static_cast<uint8_t>(loaded.refresh_preset));
  loaded.refresh_preset = refresh <= static_cast<uint8_t>(WebRefreshPreset::SMOOTH)
      ? static_cast<WebRefreshPreset>(refresh)
      : WebRefreshPreset::BALANCED;

  const uint8_t lyric = pref.getUChar("lyric", static_cast<uint8_t>(loaded.lyric_sync_mode));
  loaded.lyric_sync_mode = lyric <= static_cast<uint8_t>(WebLyricSyncMode::FOLLOW_POLL)
      ? static_cast<WebLyricSyncMode>(lyric)
      : WebLyricSyncMode::BALANCED;

  loaded.show_next_lyric = pref.getBool("show_next", loaded.show_next_lyric);
  loaded.show_cover = pref.getBool("show_cover", loaded.show_cover);
  loaded.web_cover_spin = pref.getBool("cover_spin", loaded.web_cover_spin);
  loaded.wifi_enabled = pref.getBool("wifi_en", loaded.wifi_enabled);
  loaded.show_wifi_info = pref.getBool("wifi_info", loaded.show_wifi_info);
  loaded.hall_control_enabled = pref.getBool("hall_en", loaded.hall_control_enabled);
  loaded.solenoid_enabled = pref.getBool("sol_en", loaded.solenoid_enabled);
  loaded.status_led_enabled = pref.getBool("led_en", loaded.status_led_enabled);
  const uint8_t led_bri = pref.getUChar("led_bri", static_cast<uint8_t>(loaded.status_led_brightness));
  loaded.status_led_brightness = led_bri <= static_cast<uint8_t>(StatusLedBrightness::High)
      ? static_cast<StatusLedBrightness>(led_bri)
      : StatusLedBrightness::Medium;
  pref.end();

  // 先在局部对象中完成 NVS 读取，再一次性发布，防止其它任务读到半更新状态。
  portENTER_CRITICAL(&s_settings_mux);
  s_cfg = loaded;
  s_dirty = false;
  ++s_revision;
  portEXIT_CRITICAL(&s_settings_mux);

  LOGD("[网页] 设置已从 NVS 读取：刷新=%s 歌词=%s WiFi=%d HALL=%d SOL=%d LED=%d/%s",
       web_refresh_preset_key(loaded.refresh_preset),
       web_lyric_sync_mode_key(loaded.lyric_sync_mode),
       (int)loaded.wifi_enabled,
       (int)loaded.hall_control_enabled,
       (int)loaded.solenoid_enabled,
       (int)loaded.status_led_enabled,
       status_led_brightness_key(loaded.status_led_brightness));
  return true;
}

bool web_settings_save() {
  WebRuntimeSettings snapshot{};
  uint32_t saved_revision = 0;

  // NVS 写入不能放在临界区内，只在开始时复制稳定快照和版本号。
  portENTER_CRITICAL(&s_settings_mux);
  snapshot = s_cfg;
  saved_revision = s_revision;
  portEXIT_CRITICAL(&s_settings_mux);

  Preferences pref;
  if (!pref.begin(kPrefsNs, false)) {
    LOGE("[网页] 设置保存失败：无法打开 NVS namespace");
    return false;
  }

  const bool ok = pref.putUChar("refresh", (uint8_t)snapshot.refresh_preset)
               && pref.putUChar("lyric", (uint8_t)snapshot.lyric_sync_mode)
               && pref.putBool("show_next", snapshot.show_next_lyric)
               && pref.putBool("show_cover", snapshot.show_cover)
               && pref.putBool("cover_spin", snapshot.web_cover_spin)
               && pref.putBool("wifi_en", snapshot.wifi_enabled)
               && pref.putBool("wifi_info", snapshot.show_wifi_info)
               && pref.putBool("hall_en", snapshot.hall_control_enabled)
               && pref.putBool("sol_en", snapshot.solenoid_enabled)
               && pref.putBool("led_en", snapshot.status_led_enabled)
               && pref.putUChar("led_bri", static_cast<uint8_t>(snapshot.status_led_brightness));
  pref.end();

  if (!ok) {
    LOGE("[网页] 设置保存失败：写入 NVS");
    return false;
  }

  // 保存期间若其它任务又修改了设置，不能把新修改错误清成“已保存”。
  portENTER_CRITICAL(&s_settings_mux);
  if (s_revision == saved_revision && web_settings_equal(s_cfg, snapshot)) {
    s_dirty = false;
  }
  const bool still_dirty = s_dirty;
  portEXIT_CRITICAL(&s_settings_mux);

  LOGI("[网页] 设置已保存到 NVS：刷新=%s 歌词=%s WiFi=%d HALL=%d SOL=%d LED=%d/%s%s",
       web_refresh_preset_key(snapshot.refresh_preset),
       web_lyric_sync_mode_key(snapshot.lyric_sync_mode),
       (int)snapshot.wifi_enabled,
       (int)snapshot.hall_control_enabled,
       (int)snapshot.solenoid_enabled,
       (int)snapshot.status_led_enabled,
       status_led_brightness_key(snapshot.status_led_brightness),
       still_dirty ? "（保存期间又有新修改，仍待保存）" : "");
  return true;
}
