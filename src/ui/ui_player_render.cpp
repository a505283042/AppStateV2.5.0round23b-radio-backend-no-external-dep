#include <Arduino.h>

#ifndef UI_LYRICS_STATE_DEBUG_LOG
#define UI_LYRICS_STATE_DEBUG_LOG 0
#endif

#include <esp_heap_caps.h>
#include "ui/ui_internal.h"
#include "ui/ui_text_utils.h"
#include "ui/ui_progress.h"
#include "ui/ui_colors.h"
#include "ui/ui_icons.h"
#include "ui/cover_panel_skin.h"

#include "audio/audio.h"
#include "audio/audio_service.h"
#include "lyrics/lyrics.h"
#include "player_assets.h"
#include "player_state.h"
#include "player_source.h"
#include "hal/board_hw_control.h"
#include "utils/log.h"
#include "utils/text_normalize.h"
#include "app_diagnostics.h"
#include "app_alarm.h"
#include "app_power.h"

#undef LOG_TAG
#define LOG_TAG "UI"

#ifndef UI_RENDER_PROFILING
#define UI_RENDER_PROFILING 0
#endif

#if UI_RENDER_PROFILING
struct UiRenderProfStat {
  uint32_t sum_us = 0;
  uint32_t max_us = 0;
  uint32_t count = 0;
  uint32_t last_log_ms = 0;
};

static void ui_render_prof_sample(UiRenderProfStat& st, const char* label, uint32_t dt_us)
{
  st.sum_us += dt_us;
  if (dt_us > st.max_us) st.max_us = dt_us;
  st.count++;

  const uint32_t now_ms = millis();
  if (st.count > 0 && now_ms - st.last_log_ms >= 1000) {
    Serial.printf("[UI-PROF] %-26s avg=%lu us max=%lu us frames=%lu\n",
                  label,
                  (unsigned long)(st.sum_us / st.count),
                  (unsigned long)st.max_us,
                  (unsigned long)st.count);
    st.sum_us = 0;
    st.max_us = 0;
    st.count = 0;
    st.last_log_ms = now_ms;
  }
}

#define UI_PROF_T0() micros()
#define UI_PROF_SAMPLE(stat, label, t0) ui_render_prof_sample((stat), (label), micros() - (t0))

static UiRenderProfStat s_prof_cover_rotate_total;
static UiRenderProfStat s_prof_cover_rotate_push;
static UiRenderProfStat s_prof_fullscreen_bilinear;
static UiRenderProfStat s_prof_panel_total;
static UiRenderProfStat s_prof_panel_cover;
static UiRenderProfStat s_prof_panel_record;
static UiRenderProfStat s_prof_panel_skin;
static UiRenderProfStat s_prof_panel_info;
static UiRenderProfStat s_prof_panel_progress;
static UiRenderProfStat s_prof_panel_push;
#else
#define UI_PROF_T0() 0
#define UI_PROF_SAMPLE(stat, label, t0) do {} while (0)
#endif

void cover_panel_invalidate_source_cache();

// 全屏旋转页抗锯齿开关。
// 1: 使用安全 bilinear 旋转采样，锯齿更轻，但每帧计算量高于 pushRotateZoom。
// 0: 回退 LovyanGFX pushRotateZoom 原路径。
#ifndef UI_FULLSCREEN_COVER_BILINEAR
#define UI_FULLSCREEN_COVER_BILINEAR 1
#endif

static bool draw_fullscreen_rotated_cover_bilinear(LGFX_Sprite* dst, LGFX_Sprite* src, float angle_deg);

// Overlay 状态由主循环更新、UiTask 读取。包含 String 的状态统一在 ui_lock() 内发布，
// 绘制函数只使用锁内复制出的局部快照，避免字符串重分配与渲染并发。
static void cover_set_source(LGFX_Sprite* src)
{
  s_src = src;
  cover_panel_invalidate_source_cache();
}

// =============================================================================
// 音量步进小提示 Overlay
// =============================================================================

static constexpr uint32_t VOLUME_STEP_HINT_DURATION_MS = 1200;

static uint8_t s_volume_step_hint = 1;
static uint32_t s_volume_step_hint_until_ms = 0;

struct VolumeStepHintSnapshot {
  uint8_t step = 1;
  uint32_t until_ms = 0;
};

void ui_show_volume_step_hint(uint8_t step)
{
  if (step == 0) {
    step = 1;
  }

  ui_lock();
  s_volume_step_hint = step;
  s_volume_step_hint_until_ms = millis() + VOLUME_STEP_HINT_DURATION_MS;
  ui_unlock();

  ui_request_refresh();
}

static bool volume_step_hint_snapshot(uint32_t now, VolumeStepHintSnapshot& out)
{
  ui_lock();
  out.step = s_volume_step_hint;
  out.until_ms = s_volume_step_hint_until_ms;
  ui_unlock();

  return out.until_ms != 0 &&
         static_cast<int32_t>(out.until_ms - now) > 0;
}

static void draw_volume_step_hint_overlay(LGFX_Sprite* dst)
{
  if (!dst) {
    return;
  }

  const uint32_t now = millis();
  VolumeStepHintSnapshot hint{};
  if (!volume_step_hint_snapshot(now, hint)) {
    return;
  }

  const uint8_t step = hint.step;
  const UiPlayerRuntimeSnapshot runtime = ui_player_runtime_snapshot_get();
  const uint8_t volume = runtime.volume;

  // 圆屏上中间小弹窗，显示当前音量值和本次旋钮步进。
  // 弹窗宽度收窄，避免在圆屏安全区边缘显得太长。
  static constexpr int BOX_W = 100;
  static constexpr int BOX_H = 32;
  static constexpr int BOX_X = (240 - BOX_W) / 2;
  static constexpr int BOX_Y = (240 - BOX_H) / 2;
  static constexpr int BOX_R = 9;

  const uint16_t border_color = step > 1 ? TFT_YELLOW : TFT_DARKGREY;
  const uint16_t icon_color = step > 1 ? TFT_YELLOW : TFT_LIGHTGREY;
  const uint16_t text_color = TFT_WHITE;
  const uint16_t step_color = step > 1 ? TFT_YELLOW : TFT_LIGHTGREY;

  dst->fillRoundRect(BOX_X, BOX_Y, BOX_W, BOX_H, BOX_R, TFT_BLACK);
  dst->drawRoundRect(BOX_X, BOX_Y, BOX_W, BOX_H, BOX_R, border_color);

  // 复用项目已有音量图标。
  draw_volume_icon(dst, BOX_X + 9, BOX_Y + 10, icon_color);

  char volume_label[8];
  if (runtime.volume_known) {
    snprintf(volume_label, sizeof(volume_label), "%u%%", static_cast<unsigned>(volume));
  } else {
    snprintf(volume_label, sizeof(volume_label), "--");
  }

  char step_label[8];
  snprintf(step_label, sizeof(step_label), "x%u", static_cast<unsigned>(step));

  dst->setFont(&g_font_cjk);
  dst->setTextSize(1);
  dst->setTextWrap(false);

  dst->setTextDatum(middle_left);
  dst->setTextColor(text_color, TFT_BLACK);
  dst->drawString(volume_label, BOX_X + 30, BOX_Y + BOX_H / 2);

  dst->setTextDatum(middle_right);
  dst->setTextColor(step_color, TFT_BLACK);
  dst->drawString(step_label, BOX_X + BOX_W - 9, BOX_Y + BOX_H / 2);

  dst->setTextDatum(top_left);
}


// =============================================================================
// 快退 / 快进预览 Overlay
// =============================================================================

static bool s_seek_preview_visible = false;
static bool s_seek_preview_available = true;
static int8_t s_seek_preview_direction = 1;
static uint32_t s_seek_preview_target_ms = 0;

struct SeekPreviewSnapshot {
  bool visible = false;
  bool available = true;
  int8_t direction = 1;
  uint32_t target_ms = 0;
};

void ui_show_seek_preview(int8_t direction, uint32_t target_ms)
{
  ui_lock();
  s_seek_preview_visible = true;
  s_seek_preview_available = true;
  s_seek_preview_direction = direction < 0 ? -1 : 1;
  s_seek_preview_target_ms = target_ms;
  ui_unlock();
  ui_request_refresh();
}

void ui_show_seek_unavailable()
{
  ui_lock();
  s_seek_preview_visible = true;
  s_seek_preview_available = false;
  s_seek_preview_direction = 0;
  s_seek_preview_target_ms = 0;
  ui_unlock();
  ui_request_refresh();
}

void ui_hide_seek_preview()
{
  ui_lock();
  const bool changed = s_seek_preview_visible;
  s_seek_preview_visible = false;
  ui_unlock();
  if (changed) {
    ui_request_refresh();
  }
}

static SeekPreviewSnapshot seek_preview_snapshot()
{
  SeekPreviewSnapshot out{};
  ui_lock();
  out.visible = s_seek_preview_visible;
  out.available = s_seek_preview_available;
  out.direction = s_seek_preview_direction;
  out.target_ms = s_seek_preview_target_ms;
  ui_unlock();
  return out;
}

static void draw_seek_preview_overlay(LGFX_Sprite* dst)
{
  if (!dst) {
    return;
  }

  const SeekPreviewSnapshot preview = seek_preview_snapshot();
  if (!preview.visible) {
    return;
  }

  static constexpr int BOX_W = 150;
  static constexpr int BOX_H = 46;
  static constexpr int BOX_X = (240 - BOX_W) / 2;
  static constexpr int BOX_Y = 76;
  static constexpr int BOX_R = 12;

  const uint16_t accent = preview.available ? TFT_CYAN : TFT_ORANGE;
  dst->fillRoundRect(BOX_X, BOX_Y, BOX_W, BOX_H, BOX_R, TFT_BLACK);
  dst->drawRoundRect(BOX_X, BOX_Y, BOX_W, BOX_H, BOX_R, accent);

  dst->setFont(&g_font_cjk);
  dst->setTextSize(1);
  dst->setTextWrap(false);
  dst->setTextDatum(middle_center);
  dst->setTextColor(accent, TFT_BLACK);

  if (!preview.available) {
    dst->drawString("当前音源不可跳转", 120, BOX_Y + BOX_H / 2);
    dst->setTextDatum(top_left);
    return;
  }

  dst->drawString(preview.direction < 0 ? "快退" : "快进", 120, BOX_Y + 14);

  const uint32_t total_seconds = preview.target_ms / 1000UL;
  char time_text[20];
  snprintf(time_text,
           sizeof(time_text),
           "%02lu:%02lu",
           (unsigned long)(total_seconds / 60UL),
           (unsigned long)(total_seconds % 60UL));
  dst->setTextColor(TFT_WHITE, TFT_BLACK);
  dst->drawString(time_text, 120, BOX_Y + 32);
  dst->setTextDatum(top_left);
}


// =============================================================================
// 低电量提示 Overlay
// =============================================================================

static constexpr uint8_t LOW_BATTERY_HINT_PERCENT = 15;
static constexpr uint8_t CRITICAL_BATTERY_HINT_PERCENT = 5;
static constexpr uint32_t LOW_BATTERY_HINT_INTERVAL_MS = 30000;
static constexpr uint32_t LOW_BATTERY_HINT_DURATION_MS = 3000;

static uint32_t s_low_battery_hint_window_start_ms = 0;
static bool s_low_battery_hint_was_active = false;

static void draw_low_battery_hint_overlay(LGFX_Sprite* dst)
{
    if (!dst) {
        return;
    }

    const BatteryUiStatus bat = board_hw_get_battery_status_cached();
    if (!bat.valid) {
        s_low_battery_hint_was_active = false;
        s_low_battery_hint_window_start_ms = 0;
        return;
    }

    uint8_t percent = bat.percent;
    if (percent > 100) {
        percent = 100;
    }

    // 接上外部电源或正在充电时，不再提醒低电。
    const bool plugged = bat.external_power_good;
    const bool charging = bat.charging;
    const bool active = !plugged && !charging && percent <= LOW_BATTERY_HINT_PERCENT;
    if (!active) {
        s_low_battery_hint_was_active = false;
        s_low_battery_hint_window_start_ms = 0;
        return;
    }

    const uint32_t now = millis();

    // 刚进入低电状态时立即显示一次；之后每 30 秒显示 3 秒。
    if (!s_low_battery_hint_was_active ||
        s_low_battery_hint_window_start_ms == 0 ||
        now - s_low_battery_hint_window_start_ms >= LOW_BATTERY_HINT_INTERVAL_MS) {
        s_low_battery_hint_window_start_ms = now;
        s_low_battery_hint_was_active = true;
    }

    if (now - s_low_battery_hint_window_start_ms > LOW_BATTERY_HINT_DURATION_MS) {
        return;
    }

    const bool critical = percent <= CRITICAL_BATTERY_HINT_PERCENT;
    const uint16_t border_color = critical ? TFT_RED : TFT_ORANGE;
    const uint16_t text_color = critical ? TFT_RED : TFT_ORANGE;

    static constexpr int BOX_W = 150;
    static constexpr int BOX_H = 32;
    static constexpr int BOX_X = (240 - BOX_W) / 2;
    static constexpr int BOX_Y = 104;
    static constexpr int BOX_R = 10;

    dst->fillRoundRect(BOX_X, BOX_Y, BOX_W, BOX_H, BOX_R, TFT_BLACK);
    dst->drawRoundRect(BOX_X, BOX_Y, BOX_W, BOX_H, BOX_R, border_color);

    char text[32];
    if (critical) {
        snprintf(text, sizeof(text), "电量过低 %u%%", static_cast<unsigned>(percent));
    } else {
        snprintf(text, sizeof(text), "电量低 请充电 %u%%", static_cast<unsigned>(percent));
    }

    dst->setFont(&g_font_cjk);
    dst->setTextSize(1);
    dst->setTextWrap(false);
    dst->setTextColor(text_color, TFT_BLACK);
    dst->setTextDatum(middle_center);
    dst->drawString(text, 120, BOX_Y + BOX_H / 2);
    dst->setTextDatum(top_left);
}


// =============================================================================
// 睡眠关机倒计时 Overlay
// =============================================================================

static void draw_sleep_timer_overlay(LGFX_Sprite* dst)
{
    if (!dst || !app_power_sleep_timer_is_active()) {
        return;
    }

    const uint32_t remain = app_power_sleep_timer_remaining_seconds();
    if (remain == 0) {
        return;
    }

    char text[24];
    if (remain >= 60) {
        const uint32_t minutes = (remain + 59UL) / 60UL;
        snprintf(text, sizeof(text), "睡眠 %lu分", (unsigned long)minutes);
    } else {
        snprintf(text, sizeof(text), "睡眠 %lu秒", (unsigned long)remain);
    }

    // 圆屏顶部中央相对安全，不占用底部电池图标区域。
    static constexpr int BOX_W = 104;
    static constexpr int BOX_H = 18;
    static constexpr int BOX_X = (240 - BOX_W) / 2;
    static constexpr int BOX_Y = 8;
    static constexpr int BOX_R = 8;

    const uint16_t border_color = TFT_DARKGREY;
    const uint16_t text_color = TFT_LIGHTGREY;

    dst->fillRoundRect(BOX_X, BOX_Y, BOX_W, BOX_H, BOX_R, TFT_BLACK);
    dst->drawRoundRect(BOX_X, BOX_Y, BOX_W, BOX_H, BOX_R, border_color);

    // 使用统一的 11x10 睡眠点阵图标。
    draw_sleep_icon(dst, BOX_X + 8, BOX_Y + 4, text_color);

    dst->setFont(&g_font_cjk);
    dst->setTextSize(1);
    dst->setTextWrap(false);
    dst->setTextColor(text_color, TFT_BLACK);
    dst->setTextDatum(middle_left);
    dst->drawString(text, BOX_X + 26, BOX_Y + BOX_H / 2);
    dst->setTextDatum(top_left);
}

// =============================================================================
// 全屏旋转视图切歌提示 Overlay
// =============================================================================

static constexpr uint32_t TRACK_CHANGE_POPUP_DURATION_MS = 2600;

static uint32_t s_track_change_popup_until_ms = 0;
static String s_track_change_popup_title;
static String s_track_change_popup_artist;

struct TrackChangePopupSnapshot {
  uint32_t until_ms = 0;
  String title;
  String artist;
};

template <typename CanvasT>
static String track_popup_fit_text_px(CanvasT* dst, String text, int max_w)
{
  text.trim();
  if (!dst || max_w <= 0 || text.length() == 0) {
    return text;
  }

  if (dst->textWidth(text) <= max_w) {
    return text;
  }

  const String ellipsis = "...";
  String out = text;

  // 按像素宽度裁剪，并回退到 UTF-8 字符边界，避免中文歌名被截坏。
  while (out.length() > 0 && dst->textWidth(out + ellipsis) > max_w) {
    int len = out.length();
    do {
      --len;
    } while (len > 0 && ((static_cast<uint8_t>(out[len]) & 0xC0) == 0x80));

    out = out.substring(0, len);
  }

  if (out.length() == 0) {
    return ellipsis;
  }

  return out + ellipsis;
}

static bool track_change_popup_snapshot(uint32_t now,
                                        TrackChangePopupSnapshot& out)
{
  ui_lock();
  out.until_ms = s_track_change_popup_until_ms;
  const bool active = out.until_ms != 0 &&
                      static_cast<int32_t>(out.until_ms - now) > 0;
  if (active) {
    out.title = s_track_change_popup_title;
    out.artist = s_track_change_popup_artist;
  }
  ui_unlock();
  return active;
}

void ui_show_track_change_popup(const char* title, const char* artist)
{
  String next_title = title ? String(title) : String("");
  String next_artist = artist ? String(artist) : String("");
  (void)text_normalize_display_spaces_inplace(next_title);
  (void)text_normalize_display_spaces_inplace(next_artist);
  next_title.trim();
  next_artist.trim();

  if (next_title.isEmpty()) {
    next_title = "未知歌曲";
  }
  if (next_artist.isEmpty()) {
    next_artist = "未知歌手";
  }

  ui_lock();
  s_track_change_popup_title = next_title;
  s_track_change_popup_artist = next_artist;
  s_track_change_popup_until_ms = millis() + TRACK_CHANGE_POPUP_DURATION_MS;
  ui_unlock();
  ui_request_refresh();
}

static void draw_track_change_popup_overlay(LGFX_Sprite* dst)
{
  if (!dst) {
    return;
  }

  TrackChangePopupSnapshot popup{};
  if (!track_change_popup_snapshot(millis(), popup)) {
    return;
  }

  // 只用于全屏旋转封面页。圆屏底部横向安全宽度会变窄，
  // 所以弹窗不能按 240 方屏铺太宽，否则左右下角会出圆屏。
  static constexpr int BOX_W = 184;
  static constexpr int BOX_H = 38;
  static constexpr int BOX_X = (240 - BOX_W) / 2;
  static constexpr int BOX_Y = 154;
  static constexpr int BOX_R = 10;
  static constexpr int PAD_X = 10;
  static constexpr int TEXT_MAX_W = BOX_W - PAD_X * 2;

  dst->fillRoundRect(BOX_X, BOX_Y, BOX_W, BOX_H, BOX_R, TFT_BLACK);
  dst->drawRoundRect(BOX_X, BOX_Y, BOX_W, BOX_H, BOX_R, TFT_DARKGREY);

  dst->setFont(&g_font_cjk);
  dst->setTextSize(1);
  dst->setTextWrap(false);

  const String title = track_popup_fit_text_px(dst, popup.title, TEXT_MAX_W);
  const String artist = track_popup_fit_text_px(dst, popup.artist, TEXT_MAX_W);

  dst->setTextDatum(middle_center);
  dst->setTextColor(TFT_WHITE, TFT_BLACK);
  dst->drawString(title, BOX_X + BOX_W / 2, BOX_Y + 12);

  dst->setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  dst->drawString(artist, BOX_X + BOX_W / 2, BOX_Y + 27);

  dst->setTextDatum(top_left);
}

// =============================================================================
// NFC 绑定类型选择 Overlay
// =============================================================================

static bool s_nfc_bind_target_popup_visible = false;
static uint8_t s_nfc_bind_target_popup_selected = 0;
static bool s_nfc_bind_target_popup_dirty = false;

struct NfcBindTargetPopupSnapshot {
  bool visible = false;
  uint8_t selected = 0;
};

static NfcBindTargetPopupSnapshot nfc_bind_target_popup_snapshot()
{
  NfcBindTargetPopupSnapshot snapshot{};
  ui_lock();
  snapshot.visible = s_nfc_bind_target_popup_visible;
  snapshot.selected = s_nfc_bind_target_popup_selected;
  ui_unlock();
  return snapshot;
}

bool ui_nfc_bind_target_popup_is_visible()
{
  return nfc_bind_target_popup_snapshot().visible;
}

bool ui_nfc_bind_target_popup_consume_dirty()
{
  ui_lock();
  const bool dirty = s_nfc_bind_target_popup_dirty;
  s_nfc_bind_target_popup_dirty = false;
  ui_unlock();
  return dirty;
}

template <typename CanvasT>
static void draw_nfc_bind_target_popup_canvas(CanvasT* dst, uint8_t selected)
{
  if (!dst) {
    return;
  }

  if (selected > 2) {
    selected = 0;
  }

  // 保持和音量步进提示一致的“居中黑色圆角弹窗”风格，尺寸稍大以容纳三项选择。
  static constexpr int BOX_W = 168;
  static constexpr int BOX_H = 88;
  static constexpr int BOX_X = (240 - BOX_W) / 2;
  static constexpr int BOX_Y = (240 - BOX_H) / 2;
  static constexpr int BOX_R = 14;

  dst->fillRoundRect(BOX_X, BOX_Y, BOX_W, BOX_H, BOX_R, TFT_BLACK);
  dst->drawRoundRect(BOX_X, BOX_Y, BOX_W, BOX_H, BOX_R, TFT_YELLOW);

  dst->setFont(&g_font_cjk);
  dst->setTextSize(1);
  dst->setTextWrap(false);

  dst->setTextColor(TFT_WHITE, TFT_BLACK);
  dst->setTextDatum(middle_center);
  dst->drawString("NFC绑定", BOX_X + BOX_W / 2, BOX_Y + 17);

  static const char* labels[3] = { "单曲", "歌手", "专辑" };
  static constexpr int ITEM_W = 44;
  static constexpr int ITEM_H = 24;
  static constexpr int ITEM_GAP = 7;
  static constexpr int ITEMS_W = ITEM_W * 3 + ITEM_GAP * 2;
  static constexpr int ITEM_X0 = BOX_X + (BOX_W - ITEMS_W) / 2;
  static constexpr int ITEM_Y = BOX_Y + 34;

  for (uint8_t i = 0; i < 3; ++i) {
    const int x = ITEM_X0 + i * (ITEM_W + ITEM_GAP);
    const bool active = (i == selected);
    const uint16_t border = active ? TFT_YELLOW : TFT_DARKGREY;
    const uint16_t fg = active ? TFT_YELLOW : TFT_LIGHTGREY;

    if (active) {
      dst->fillRoundRect(x, ITEM_Y, ITEM_W, ITEM_H, 8, TFT_DARKGREY);
    }

    dst->drawRoundRect(x, ITEM_Y, ITEM_W, ITEM_H, 8, border);
    dst->setTextColor(fg, active ? TFT_DARKGREY : TFT_BLACK);
    dst->drawString(labels[i], x + ITEM_W / 2, ITEM_Y + ITEM_H / 2);
  }

  dst->setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  dst->drawString("旋钮选择  PLAY确认", BOX_X + BOX_W / 2, BOX_Y + 72);
  dst->setTextDatum(top_left);
}

void ui_show_nfc_bind_target_popup(uint8_t selected)
{
  if (selected > 2) {
    selected = 0;
  }

  ui_lock();
  s_nfc_bind_target_popup_selected = selected;
  s_nfc_bind_target_popup_visible = true;
  s_nfc_bind_target_popup_dirty = true;
  ui_unlock();
  ui_request_refresh();
}

void ui_hide_nfc_bind_target_popup()
{
  ui_lock();
  s_nfc_bind_target_popup_visible = false;
  s_nfc_bind_target_popup_dirty = true;
  ui_unlock();
  ui_request_refresh();
}

void ui_draw_nfc_bind_target_popup_on_tft_if_visible()
{
  const NfcBindTargetPopupSnapshot popup = nfc_bind_target_popup_snapshot();
  if (!popup.visible) {
    return;
  }

  draw_nfc_bind_target_popup_canvas(&tft, popup.selected);
}

static void draw_nfc_bind_target_popup_overlay(LGFX_Sprite* dst)
{
  if (!dst) {
    return;
  }

  const NfcBindTargetPopupSnapshot popup = nfc_bind_target_popup_snapshot();
  if (!popup.visible) {
    return;
  }

  draw_nfc_bind_target_popup_canvas(dst, popup.selected);
}

// =============================================================================
// NFC 刷卡结果 Overlay
// =============================================================================

static constexpr uint32_t NFC_SCAN_POPUP_DURATION_MS = 4200; // 弹窗时间 4.2 秒

static bool s_nfc_scan_popup_visible = false;
static bool s_nfc_scan_popup_dirty = false;
static uint32_t s_nfc_scan_popup_until_ms = 0;
static bool s_nfc_scan_popup_bound = false;
static String s_nfc_scan_popup_uid;
static String s_nfc_scan_popup_card_type;
static String s_nfc_scan_popup_bind_type;
static String s_nfc_scan_popup_bind_name;

struct NfcScanPopupSnapshot {
  bool visible = false;
  bool bound = false;
  uint32_t until_ms = 0;
  String uid;
  String card_type;
  String bind_type;
  String bind_name;
};

static String nfc_popup_fit_text(String text, size_t max_chars)
{
  text.trim();
  if (text.length() <= max_chars) {
    return text;
  }

  if (max_chars <= 3) {
    return text.substring(0, max_chars);
  }

  String out = text.substring(0, max_chars - 3);
  out += "...";
  return out;
}

template <typename CanvasT>
static String nfc_popup_fit_text_px(CanvasT* dst, String text, int max_w)
{
  text.trim();
  if (!dst || max_w <= 0 || text.length() == 0) {
    return text;
  }

  if (dst->textWidth(text) <= max_w) {
    return text;
  }

  const String ellipsis = "...";
  String out = text;

  // 按像素宽度裁剪，并回退到 UTF-8 字符边界，避免中文歌名被截坏。
  while (out.length() > 0 && dst->textWidth(out + ellipsis) > max_w) {
    int len = out.length();
    do {
      --len;
    } while (len > 0 && ((static_cast<uint8_t>(out[len]) & 0xC0) == 0x80));

    out = out.substring(0, len);
  }

  if (out.length() == 0) {
    return ellipsis;
  }

  return out + ellipsis;
}

// =============================================================================
// NFC 操作提示 Overlay
// =============================================================================

static constexpr uint32_t NFC_NOTICE_POPUP_DURATION_MS = 3200;

static bool s_nfc_notice_popup_visible = false;
static bool s_nfc_notice_popup_dirty = false;
static uint32_t s_nfc_notice_popup_until_ms = 0;
static String s_nfc_notice_popup_title;
static String s_nfc_notice_popup_detail;

struct NfcNoticePopupSnapshot {
  bool visible = false;
  String title;
  String detail;
};

static bool nfc_notice_popup_update_active_locked(uint32_t now)
{
  if (!s_nfc_notice_popup_visible) {
    return false;
  }

  if (static_cast<int32_t>(s_nfc_notice_popup_until_ms - now) > 0) {
    return true;
  }

  s_nfc_notice_popup_visible = false;
  s_nfc_notice_popup_dirty = true;
  return false;
}

static NfcNoticePopupSnapshot nfc_notice_popup_snapshot(uint32_t now)
{
  NfcNoticePopupSnapshot snapshot{};
  ui_lock();
  snapshot.visible = nfc_notice_popup_update_active_locked(now);
  if (snapshot.visible) {
    snapshot.title = s_nfc_notice_popup_title;
    snapshot.detail = s_nfc_notice_popup_detail;
  }
  ui_unlock();
  return snapshot;
}

bool ui_nfc_notice_popup_is_visible()
{
  return nfc_notice_popup_snapshot(millis()).visible;
}

bool ui_nfc_notice_popup_consume_dirty()
{
  ui_lock();
  const bool dirty = s_nfc_notice_popup_dirty;
  s_nfc_notice_popup_dirty = false;
  ui_unlock();
  return dirty;
}

template <typename CanvasT>
static void draw_nfc_notice_popup_canvas(CanvasT* dst)
{
  if (!dst) {
    return;
  }

  const NfcNoticePopupSnapshot popup = nfc_notice_popup_snapshot(millis());
  if (!popup.visible) {
    return;
  }

  static constexpr int BOX_W = 200;
  static constexpr int BOX_H = 66;
  static constexpr int BOX_X = (240 - BOX_W) / 2;
  static constexpr int BOX_Y = (240 - BOX_H) / 2;
  static constexpr int BOX_R = 14;
  static constexpr int TEXT_W = BOX_W - 20;

  dst->fillRoundRect(BOX_X, BOX_Y, BOX_W, BOX_H, BOX_R, TFT_BLACK);
  dst->drawRoundRect(BOX_X, BOX_Y, BOX_W, BOX_H, BOX_R, TFT_ORANGE);

  dst->setFont(&g_font_cjk);
  dst->setTextSize(1);
  dst->setTextWrap(false);
  dst->setTextDatum(middle_center);

  dst->setTextColor(TFT_ORANGE, TFT_BLACK);
  dst->drawString(nfc_popup_fit_text_px(dst, popup.title, TEXT_W),
                  BOX_X + BOX_W / 2,
                  BOX_Y + 22);

  dst->setTextColor(TFT_WHITE, TFT_BLACK);
  dst->drawString(nfc_popup_fit_text_px(dst, popup.detail, TEXT_W),
                  BOX_X + BOX_W / 2,
                  BOX_Y + 45);

  dst->setTextDatum(top_left);
}

void ui_show_nfc_notice_popup(const char* title, const char* detail)
{
  String next_title = title ? String(title) : String("");
  String next_detail = detail ? String(detail) : String("");
  next_title.trim();
  next_detail.trim();

  if (next_title.isEmpty()) {
    next_title = "NFC提示";
  }
  if (next_detail.isEmpty()) {
    next_detail = "操作不可用";
  }

  ui_lock();
  s_nfc_notice_popup_title = next_title;
  s_nfc_notice_popup_detail = next_detail;
  s_nfc_notice_popup_visible = true;
  s_nfc_notice_popup_dirty = true;
  s_nfc_notice_popup_until_ms = millis() + NFC_NOTICE_POPUP_DURATION_MS;
  ui_unlock();
  ui_request_refresh();
}

void ui_draw_nfc_notice_popup_on_tft_if_visible()
{
  draw_nfc_notice_popup_canvas(&tft);
}

static void draw_nfc_notice_popup_overlay(LGFX_Sprite* dst)
{
  draw_nfc_notice_popup_canvas(dst);
}

static bool nfc_scan_popup_update_active_locked(uint32_t now)
{
  if (!s_nfc_scan_popup_visible) {
    return false;
  }

  if (static_cast<int32_t>(s_nfc_scan_popup_until_ms - now) > 0) {
    return true;
  }

  s_nfc_scan_popup_visible = false;
  s_nfc_scan_popup_dirty = true;
  return false;
}

static bool nfc_scan_popup_snapshot(uint32_t now, NfcScanPopupSnapshot& out)
{
  ui_lock();
  const bool active = nfc_scan_popup_update_active_locked(now);
  if (active) {
    out.visible = true;
    out.bound = s_nfc_scan_popup_bound;
    out.until_ms = s_nfc_scan_popup_until_ms;
    out.uid = s_nfc_scan_popup_uid;
    out.card_type = s_nfc_scan_popup_card_type;
    out.bind_type = s_nfc_scan_popup_bind_type;
    out.bind_name = s_nfc_scan_popup_bind_name;
  }
  ui_unlock();
  return active;
}

bool ui_nfc_scan_popup_is_visible()
{
  ui_lock();
  const bool active = nfc_scan_popup_update_active_locked(millis());
  ui_unlock();
  return active;
}

bool ui_nfc_scan_popup_consume_dirty()
{
  ui_lock();
  const bool dirty = s_nfc_scan_popup_dirty;
  s_nfc_scan_popup_dirty = false;
  ui_unlock();
  return dirty;
}

void ui_show_nfc_scan_popup(const String& uid,
                            const String& card_type,
                            const String& bind_type,
                            const String& bind_name,
                            bool bound)
{
  String next_uid = uid;
  String next_card_type = card_type;
  String next_bind_type = bind_type;
  String next_bind_name = bind_name;
  next_uid.trim();
  next_card_type.trim();
  next_bind_type.trim();
  next_bind_name.trim();

  if (next_card_type.isEmpty()) {
    next_card_type = "未知卡";
  }
  if (next_bind_type.isEmpty()) {
    next_bind_type = bound ? String("已绑定") : String("未绑定");
  }
  if (!bound && next_bind_name.isEmpty()) {
    next_bind_name = "按住旋钮+长按上一曲绑定";
  }

  ui_lock();
  s_nfc_scan_popup_uid = next_uid;
  s_nfc_scan_popup_card_type = next_card_type;
  s_nfc_scan_popup_bind_type = next_bind_type;
  s_nfc_scan_popup_bind_name = next_bind_name;
  s_nfc_scan_popup_bound = bound;
  s_nfc_scan_popup_visible = true;
  s_nfc_scan_popup_dirty = true;
  s_nfc_scan_popup_until_ms = millis() + NFC_SCAN_POPUP_DURATION_MS;
  ui_unlock();
  ui_request_refresh();
}

template <typename CanvasT>
static void draw_nfc_scan_popup_canvas(CanvasT* dst)
{
  if (!dst) {
    return;
  }

  NfcScanPopupSnapshot popup{};
  if (!nfc_scan_popup_snapshot(millis(), popup)) {
    return;
  }

  // 居中黑色圆角弹窗，和音量/NFC绑定弹窗保持一致风格。
  // 标题和绑定状态合并到一行后，高度可以从 118px 收到 100px。
  static constexpr int BOX_W = 220;
  static constexpr int BOX_H = 100;
  static constexpr int BOX_X = (240 - BOX_W) / 2;
  static constexpr int BOX_Y = (240 - BOX_H) / 2;
  static constexpr int BOX_R = 14;

  const uint16_t status_color = popup.bound ? TFT_GREEN : TFT_ORANGE;

  dst->fillRoundRect(BOX_X, BOX_Y, BOX_W, BOX_H, BOX_R, TFT_BLACK);
  dst->drawRoundRect(BOX_X, BOX_Y, BOX_W, BOX_H, BOX_R, status_color);

  dst->setFont(&g_font_cjk);
  dst->setTextSize(1);
  dst->setTextWrap(false);

  dst->setTextDatum(middle_center);
  dst->setTextColor(status_color, TFT_BLACK);
  dst->drawString(popup.bound ? "NFC卡  已绑定" : "NFC卡  未绑定",
                  BOX_X + BOX_W / 2,
                  BOX_Y + 15);

  dst->setTextDatum(middle_left);
  dst->setTextColor(TFT_LIGHTGREY, TFT_BLACK);

  const int left = BOX_X + 12;
  const int value_left = left + 38;

  // 标题位置不动；下面几行每段间距各收 1px，让底部歌名离下边框更舒服。
  static constexpr int Y_UID   = 35;
  static constexpr int Y_CARD  = 52;
  static constexpr int Y_TYPE  = 69;
  static constexpr int Y_NAME  = 87;

  dst->drawString("UID:", left, BOX_Y + Y_UID);
  dst->drawString(nfc_popup_fit_text(popup.uid, 30), value_left, BOX_Y + Y_UID);

  dst->drawString("卡:", left, BOX_Y + Y_CARD);
  dst->drawString(nfc_popup_fit_text(popup.card_type, 22), value_left, BOX_Y + Y_CARD);

  dst->drawString("类型:", left, BOX_Y + Y_TYPE);
  dst->setTextColor(status_color, TFT_BLACK);
  dst->drawString(nfc_popup_fit_text(popup.bind_type, 20), value_left, BOX_Y + Y_TYPE);

  // 绑定名称/歌名限制在弹窗框内：按像素宽度裁剪，超出才加省略号。
  // 注意这里不用固定“字符数”截断，避免中文歌名明明放得下却被提前省略。
  static constexpr int NAME_PAD_X = 12;
  const int name_max_w = BOX_W - NAME_PAD_X * 2;
  const String name_text = nfc_popup_fit_text_px(dst, popup.bind_name, name_max_w);

  dst->setTextDatum(middle_center);
  dst->setTextColor(TFT_WHITE, TFT_BLACK);
  dst->drawString(name_text, BOX_X + BOX_W / 2, BOX_Y + Y_NAME);

  dst->setTextDatum(top_left);
}

void ui_draw_nfc_scan_popup_on_tft_if_visible()
{
  draw_nfc_scan_popup_canvas(&tft);
}

static void draw_nfc_scan_popup_overlay(LGFX_Sprite* dst)
{
  draw_nfc_scan_popup_canvas(dst);
}

// =============================================================================
// 电池状态页脚绘制
// =============================================================================

static void draw_tiny_battery_icon(LGFX_Sprite* dst,
                                   int x,
                                   int y,
                                   uint8_t percent,
                                   bool external_power_good,
                                   bool charging,
                                   uint16_t color)
{
    static constexpr int BODY_W = 16;
    static constexpr int BODY_H = 8;
    static constexpr int HEAD_W = 2;
    static constexpr int HEAD_H = 4;

    // 电池外框
    dst->drawRect(x, y, BODY_W, BODY_H, color);
    dst->fillRect(x + BODY_W, y + 2, HEAD_W, HEAD_H, color);

    // 电量填充
    const int inner_w = BODY_W - 4;
    const int fill_w = static_cast<int>((inner_w * percent) / 100);

    if (fill_w > 0) {
        dst->fillRect(x + 2, y + 2, fill_w, BODY_H - 4, color);
    }

    if (external_power_good) {
        // 有输入电源就显示闪电：
        // 正在充电用黄色，已接电但未充电/满电用当前状态色。
        const uint16_t lightning_color = charging ? TFT_YELLOW : color;

        const int bx = x + BODY_W + HEAD_W + 1;
        const int by = y - 1;

        dst->drawLine(bx + 3, by,     bx,     by + 5, lightning_color);
        dst->drawLine(bx,     by + 5, bx + 4, by + 5, lightning_color);
        dst->drawLine(bx + 4, by + 5, bx + 1, by + 10, lightning_color);
    }
}

void ui_draw_battery_footer(LGFX_Sprite* dst)
{
    if (!dst) {
        return;
    }

    const BatteryUiStatus bat = board_hw_get_battery_status_cached();
    if (!bat.valid) {
        return;
    }

    uint8_t percent = bat.percent;
    if (percent > 100) {
        percent = 100;
    }

    const bool plugged = bat.external_power_good;
    const bool charging = bat.charging;
    const bool low = !plugged && percent <= 15;
    const bool critical = !plugged && percent <= 5;

    uint16_t color = TFT_LIGHTGREY;
    if (charging) {
        // 正在充电时用黄色，配合闪电图标更直观。
        color = TFT_YELLOW;
    } else if (plugged) {
        // 已接外部电源但不在充电，例如满电或充电芯片停止充电。
        color = TFT_CYAN;
    } else if (critical) {
        color = TFT_RED;
    } else if (low) {
        color = TFT_ORANGE;
    }

    // 极低电时不再只画小图标，直接在底部显示明确提示。
    if (critical) {
        static constexpr int BOX_W = 86;
        static constexpr int BOX_H = 18;
        static constexpr int BOX_X = (240 - BOX_W) / 2;
        static constexpr int BOX_Y = 217;

        dst->fillRoundRect(BOX_X, BOX_Y, BOX_W, BOX_H, 8, TFT_BLACK);
        dst->drawRoundRect(BOX_X, BOX_Y, BOX_W, BOX_H, 8, TFT_RED);

        char warn[16];
        snprintf(warn, sizeof(warn), "低电 %u%%", static_cast<unsigned>(percent));

        dst->setFont(&g_font_cjk);
        dst->setTextSize(1);
        dst->setTextWrap(false);
        dst->setTextColor(TFT_RED, TFT_BLACK);
        dst->setTextDatum(middle_center);
        dst->drawString(warn, 120, BOX_Y + BOX_H / 2);
        dst->setTextDatum(top_left);
        return;
    }

    // 正常电量时尽量少占空间：只显示小图标。
    // 接电、充电、低电或 30% 以下时，才显示百分比。
    const bool show_percent = plugged || charging || low || percent <= 30;

    static constexpr int Y = 220;
    // 电池图标约 18px，闪电约 5px，中间 1px 间隔。
    // ICON_W 包含：电池 + 闪电 + 1px 余量。
    static constexpr int ICON_W = 24;
    static constexpr int TEXT_W = 28;
    static constexpr int GAP = 1;
    const int total_w = show_percent ? (ICON_W + GAP + TEXT_W) : ICON_W;
    const int x = (240 - total_w) / 2;

    draw_tiny_battery_icon(dst,
                           x,
                           Y,
                           percent,
                           plugged,
                           charging,
                           color);

    if (!show_percent) {
        return;
    }

    char text[8];
    snprintf(text, sizeof(text), "%u%%", static_cast<unsigned>(percent));

    dst->setFont(&g_font_cjk);
    dst->setTextSize(1);
    dst->setTextWrap(false);
    dst->setTextColor(color);
    dst->setTextDatum(middle_left);
    dst->drawString(text, x + ICON_W + GAP, Y + 4);
    dst->setTextDatum(top_left);
}

// =============================================================================
// 收音机闹钟状态 / 开机提示 Overlay
// =============================================================================

static constexpr uint32_t ALARM_WAKEUP_POPUP_DURATION_MS = 5000;

static uint32_t s_alarm_wakeup_popup_until_ms = 0;
static String s_alarm_wakeup_popup_title;
static String s_alarm_wakeup_popup_detail;

struct AlarmWakeupPopupSnapshot {
  uint32_t until_ms = 0;
  String title;
  String detail;
};

static const char* alarm_repeat_short_label(AppAlarmRepeatMode mode)
{
    switch (mode) {
        case AppAlarmRepeatMode::ONCE:     return "单次";
        case AppAlarmRepeatMode::DAILY:    return "闹钟";
        case AppAlarmRepeatMode::WEEKDAYS: return "工作日";
        case AppAlarmRepeatMode::WEEKENDS: return "周末";
        case AppAlarmRepeatMode::WEEKLY:   return "每周";
        default:                           return "闹钟";
    }
}

void ui_show_alarm_wakeup_popup(const char* title, const char* detail)
{
    String next_title = title ? String(title) : String("闹钟已响");
    String next_detail = detail ? String(detail) : String("");
    next_title.trim();
    next_detail.trim();

    if (next_title.isEmpty()) {
        next_title = "闹钟已响";
    }
    if (next_detail.isEmpty()) {
        next_detail = "正在处理";
    }

    ui_lock();
    s_alarm_wakeup_popup_title = next_title;
    s_alarm_wakeup_popup_detail = next_detail;
    s_alarm_wakeup_popup_until_ms = millis() + ALARM_WAKEUP_POPUP_DURATION_MS;
    ui_unlock();
    ui_request_refresh();
}

static bool alarm_wakeup_popup_snapshot(uint32_t now,
                                        AlarmWakeupPopupSnapshot& out)
{
    ui_lock();
    out.until_ms = s_alarm_wakeup_popup_until_ms;
    const bool active = out.until_ms != 0 &&
                        static_cast<int32_t>(out.until_ms - now) > 0;
    if (active) {
        out.title = s_alarm_wakeup_popup_title;
        out.detail = s_alarm_wakeup_popup_detail;
    }
    ui_unlock();
    return active;
}

static void draw_alarm_status_overlay(LGFX_Sprite* dst, int box_y = -1)
{
    if (!dst || !app_alarm_is_enabled()) {
        return;
    }

    const AppAlarmConfig cfg = app_alarm_get_config();
    char text[24];
    snprintf(text,
             sizeof(text),
             "%s %02u:%02u",
             alarm_repeat_short_label(cfg.repeat_mode),
             static_cast<unsigned>(cfg.hour),
             static_cast<unsigned>(cfg.minute));

    static constexpr int BOX_W = 108;
    static constexpr int BOX_H = 18;
    static constexpr int BOX_X = (240 - BOX_W) / 2;
    static constexpr int BOX_R = 7;

    // 圆屏中心 120、半径约 120。状态条宽 108，左右边缘 x=66/174，
    // 对应圆形安全顶部约 y=13。正常放在 y=16，既不出界也不压歌词。
    // 如果睡眠倒计时已显示在 y=8..26，闹钟状态下移到 y=28，保留 2px 间隔。
    if (box_y < 0) {
        box_y = app_power_sleep_timer_is_active() ? 28 : 16;
    }

    const uint16_t border_color = TFT_DARKGREY;
    const uint16_t text_color = TFT_LIGHTGREY;

    dst->fillRoundRect(BOX_X, box_y, BOX_W, BOX_H, BOX_R, TFT_BLACK);
    dst->drawRoundRect(BOX_X, box_y, BOX_W, BOX_H, BOX_R, border_color);

    // 图标和文字共用同一个 18px 高底纹；文字使用透明背景，
    // 避免字体渲染器额外画出一个与图标区域高度不同的矩形底纹。
    draw_alarm_icon(dst, BOX_X + 8, box_y + 4, text_color);

    dst->setFont(&g_font_cjk);
    dst->setTextSize(1);
    dst->setTextWrap(false);
    dst->setTextColor(text_color);
    dst->setTextDatum(middle_left);
    dst->drawString(text, BOX_X + 24, box_y + BOX_H / 2);
    dst->setTextDatum(top_left);
}

static void draw_alarm_wakeup_popup_overlay(LGFX_Sprite* dst)
{
    if (!dst) {
        return;
    }

    AlarmWakeupPopupSnapshot popup{};
    if (!alarm_wakeup_popup_snapshot(millis(), popup)) {
        return;
    }

    static constexpr int BOX_W = 166;
    static constexpr int BOX_H = 46;
    static constexpr int BOX_X = (240 - BOX_W) / 2;
    static constexpr int BOX_Y = 76;
    static constexpr int BOX_R = 12;

    const uint16_t border_color = TFT_YELLOW;
    const uint16_t title_color = TFT_YELLOW;
    const uint16_t detail_color = TFT_WHITE;

    dst->fillRoundRect(BOX_X, BOX_Y, BOX_W, BOX_H, BOX_R, TFT_BLACK);
    dst->drawRoundRect(BOX_X, BOX_Y, BOX_W, BOX_H, BOX_R, border_color);

    dst->setFont(&g_font_cjk);
    dst->setTextSize(1);
    dst->setTextWrap(false);
    dst->setTextDatum(middle_center);

    dst->setTextColor(title_color, TFT_BLACK);
    dst->drawString(popup.title, 120, BOX_Y + 15);

    dst->setTextColor(detail_color, TFT_BLACK);
    dst->drawString(popup.detail, 120, BOX_Y + 32);
    dst->setTextDatum(top_left);
}

// 将旋转的封面渲染到后帧并推送到 LCD（稳定路径）
// 参数: angle_deg - 旋转角度（度）
// 功能: 将源精灵旋转指定角度后绘制到后帧，然后推送到屏幕
//      使用双缓冲技术避免闪烁，交换前后帧索引
void cover_rotate_draw(float angle_deg)
{
  const uint32_t prof_total_t0 = UI_PROF_T0();

  // 检查旋转帧是否已初始化以及源精灵是否有效
  if (!s_rotFramesInited || !s_src) return;

  // 获取后帧指针（旋转专用双缓冲）
  auto* dst = s_rotFrame[s_rotBack];
  // 清空后帧
  dst->fillScreen(TFT_BLACK);

  // 封面旋转关闭时会传入 0 度。
  // 0 度不需要走 pushRotateZoom，直接推原图，降低 UI 压力。
  if (angle_deg == 0.0f) {
    s_src->pushSprite(dst, 0, 0);
  } else {
  #if UI_FULLSCREEN_COVER_BILINEAR
    // 安全 bilinear 路径：不直接写 Sprite getBuffer()，避免颜色格式/字节序导致花屏。
    // 如果缓存分配失败，会自动回退到原来的 pushRotateZoom。
    if (!draw_fullscreen_rotated_cover_bilinear(dst, s_src, angle_deg)) {
      s_src->pushRotateZoom(dst, COVER_SIZE / 2, COVER_SIZE / 2, angle_deg, 1.0f, 1.0f);
    }
  #else
    // 原稳定路径：性能好，但旋转边缘锯齿较明显。
    s_src->pushRotateZoom(dst, COVER_SIZE / 2, COVER_SIZE / 2, angle_deg, 1.0f, 1.0f);
  #endif
  }

  // 全屏旋转页保持纯封面展示，不显示常驻电池/闹钟/睡眠状态。
  // 这里只保留临时提醒：切歌、低电、闹钟开机、音量和 NFC 弹窗。
  draw_track_change_popup_overlay(dst);
  draw_low_battery_hint_overlay(dst);
  draw_alarm_wakeup_popup_overlay(dst);
  draw_volume_step_hint_overlay(dst);
  draw_seek_preview_overlay(dst);
  draw_nfc_bind_target_popup_overlay(dst);
  draw_nfc_notice_popup_overlay(dst);
  draw_nfc_scan_popup_overlay(dst);

  // 将后帧推送到屏幕 (0, 0) 位置
  const uint32_t prof_push_t0 = UI_PROF_T0();
  dst->pushSprite(0, 0);
  UI_PROF_SAMPLE(s_prof_cover_rotate_push, "cover_rotate.push", prof_push_t0);
  UI_PROF_SAMPLE(s_prof_cover_rotate_total, "cover_rotate.total", prof_total_t0);

  // 交换前后帧索引（双缓冲）
  uint8_t tmp = s_rotFront;
  s_rotFront = s_rotBack;
  s_rotBack = tmp;
}


static void draw_cover_panel_status_icons(LGFX_Sprite* dst, int center_y, uint16_t fg)
{
  if (!dst) return;

  const UiPlayerRuntimeSnapshot runtime = ui_player_runtime_snapshot_get();

  dst->setFont(&g_font_cjk);
  dst->setTextSize(1);
  dst->setTextWrap(false);

  const int panel_x = 8;
  const int panel_w = 224;
  const int margin = 4;
  const int icon_size = 10;

  const bool volume_active =
      (millis() - runtime.volume_active_time) < VOLUME_ACTIVE_TIMEOUT_MS;

  const uint16_t volume_color =
      volume_active ? UI_COLOR_VOLUME_ACTIVE : fg;

  // 左侧音量：贴近面板左边
  const int vol_icon_x = panel_x + margin;
  const int vol_icon_y = center_y - 5;

  draw_volume_icon(dst, vol_icon_x, vol_icon_y, volume_color);

  char vol_str[8];
  if (runtime.volume_known) {
    snprintf(vol_str, sizeof(vol_str), "%u%%", (unsigned)runtime.volume);
  } else {
    snprintf(vol_str, sizeof(vol_str), "--");
  }

  dst->setTextColor(volume_color);
  dst->setCursor(vol_icon_x + 15, center_y - 8);
  dst->print(vol_str);

  // 右侧模式：贴近面板右边
  const uint32_t now = millis();
  const bool mode_highlight =
      (now - runtime.mode_switch_time) < MODE_SWITCH_HIGHLIGHT_MS;

  const uint16_t mode_color =
      mode_highlight ? UI_COLOR_VOLUME_ACTIVE : fg;

  const int right_icon_x = panel_x + panel_w - margin - icon_size;
  const int icon_y = center_y - 5;

  const PlayerSourceType source_type = player_source_type_get();
  const bool is_nas = source_type == PlayerSourceType::NET_TRACK;
  const bool is_radio = source_type == PlayerSourceType::NET_RADIO;
  const int source_icon_w = is_nas ? 11 : icon_size;
  const int left_icon_x = right_icon_x - source_icon_w - 4;

  // 闹钟启用时，只在播放来源/循环模式图标左边显示统一的 10x10 图标。
  if (app_alarm_is_enabled()) {
    const int alarm_icon_x = left_icon_x - icon_size - 8;
    draw_alarm_icon(dst, alarm_icon_x, icon_y, fg);
  }

  // NAS 和网络收音机没有“歌手/专辑”播放大类。左侧固定显示当前网络来源，
  // 右侧仍保留顺序/随机状态。
  if (is_nas || is_radio) {
    if (is_nas) {
      draw_nas_icon(dst, left_icon_x, icon_y, mode_color);
    } else {
      draw_radio_icon(dst, left_icon_x, icon_y, mode_color);
    }

    const bool random_mode =
        runtime.play_mode == PLAY_MODE_ALL_RND ||
        runtime.play_mode == PLAY_MODE_ARTIST_RND ||
        runtime.play_mode == PLAY_MODE_ALBUM_RND;
    if (random_mode) {
      draw_random_icon(dst, right_icon_x, icon_y, mode_color);
    } else {
      draw_repeat_icon(dst, right_icon_x, icon_y, mode_color);
    }
    return;
  }

  switch (runtime.play_mode) {
    case PLAY_MODE_ALL_SEQ:
      draw_tfcard_icon(dst, left_icon_x, icon_y, mode_color);
      draw_repeat_icon(dst, right_icon_x, icon_y, mode_color);
      break;

    case PLAY_MODE_ALL_RND:
      draw_tfcard_icon(dst, left_icon_x, icon_y, mode_color);
      draw_random_icon(dst, right_icon_x, icon_y, mode_color);
      break;

    case PLAY_MODE_ARTIST_SEQ:
      draw_artist_icon(dst, left_icon_x, icon_y, mode_color);
      draw_repeat_icon(dst, right_icon_x, icon_y, mode_color);
      break;

    case PLAY_MODE_ARTIST_RND:
      draw_artist_icon(dst, left_icon_x, icon_y, mode_color);
      draw_random_icon(dst, right_icon_x, icon_y, mode_color);
      break;

    case PLAY_MODE_ALBUM_SEQ:
      draw_album_icon(dst, left_icon_x, icon_y, mode_color);
      draw_repeat_icon(dst, right_icon_x, icon_y, mode_color);
      break;

    case PLAY_MODE_ALBUM_RND:
      draw_album_icon(dst, left_icon_x, icon_y, mode_color);
      draw_random_icon(dst, right_icon_x, icon_y, mode_color);
      break;
  }
}

// =============================================================================
// COVER_PANEL：上半圆旋转采样 + 下半固定信息面板
// =============================================================================
// =============================================================================
// COVER_PANEL 布局参数
// =============================================================================
// 面板从哪里开始盖住封面。
// 数值越大：上半封面越多，下面板越少。
// 建议调节范围：104 ~ 116。
static constexpr int COVER_PANEL_Y = 124;

// 面板皮肤贴图起始 Y
static constexpr int COVER_PANEL_SKIN_Y = 100;

// 文本安全边距。
// 给时间等中部内容用。
static constexpr int COVER_PANEL_SAFE_PAD = 12;

// 第一行基础 Y：音量模式，上下一曲，播放按键位置 控制区参考线
static constexpr int COVER_PANEL_CTRL_Y = 127;

// 控制按钮尺寸
static constexpr int COVER_PANEL_PLAY_R = 16;
static constexpr int COVER_PANEL_SIDE_R = 10;

// 播放按钮中心 Y
static constexpr int COVER_PANEL_PLAY_Y = COVER_PANEL_CTRL_Y;

// 上一曲 / 下一曲中心 Y
// 小按钮底部和播放按钮底部对齐
static constexpr int COVER_PANEL_SIDE_Y =
    COVER_PANEL_PLAY_Y + (COVER_PANEL_PLAY_R - COVER_PANEL_SIDE_R);

// 上一曲 / 下一曲 X 坐标。
// 数值越靠近 120，按钮越靠中间。
static constexpr int COVER_PANEL_PREV_X = 93;
static constexpr int COVER_PANEL_NEXT_X = 147;

// 播放按钮 X 坐标。
static constexpr int COVER_PANEL_PLAY_X = 120;

// 导航反馈：上/下/无
static int8_t s_cover_panel_nav_feedback = 0;
static uint32_t s_cover_panel_nav_feedback_until_ms = 0;

// 通知封面面板导航反馈（上/下）
// 导航反馈会持续 320ms，超过时间后自动清除。
void ui_notify_cover_panel_nav_feedback(int8_t dir)
{
  ui_lock();
  if (dir < 0) {
    s_cover_panel_nav_feedback = -1;
  } else if (dir > 0) {
    s_cover_panel_nav_feedback = 1;
  } else {
    s_cover_panel_nav_feedback = 0;
  }

  // COVER_PANEL 帧率可能只有 8FPS，时间太短可能看不到
  s_cover_panel_nav_feedback_until_ms = millis() + 320;
  ui_unlock();
}

// 音量 / 模式跟上一曲 / 下一曲中心对齐
static constexpr int COVER_PANEL_STATUS_Y = COVER_PANEL_SIDE_Y;

// 第二行：时间
static constexpr int COVER_PANEL_TIME_Y = 148;

// 第三行：歌名
static constexpr int COVER_PANEL_TITLE_Y = 166;

// 第四行：当前歌词
static constexpr int COVER_PANEL_LYRIC_CUR_Y = 184;

// 第五行：下一句歌词
static constexpr int COVER_PANEL_LYRIC_NEXT_Y = 202;

// 歌名 / 歌词专用安全边距。
// 给未来外圈进度弧预留空间。
static constexpr int COVER_PANEL_TEXT_SAFE_PAD = 18;
// 面板内部半径。
// 240 圆屏半径约 120，这里用 114 是为了给外圈进度弧预留约 6px。
static constexpr int COVER_PANEL_INNER_R = 114;

// 外圈进度弧半径。
// 屏幕半径约 120，面板内部半径 114，所以 116~117 比较合适。
static constexpr int COVER_PANEL_PROGRESS_R = 116;

// 外圈进度弧厚度。
static constexpr int COVER_PANEL_PROGRESS_THICKNESS = 3;

// 外圈进度弧角度范围。
// 0 度 = 顶部，90 = 右侧，180 = 底部，270 = 左侧。
// 因此 100 -> 260 是下方弧形区域。
// 下方弧形区域：左侧到右侧的弧形区域。
static constexpr int COVER_PANEL_PROGRESS_LEFT_DEG  = 260;
static constexpr int COVER_PANEL_PROGRESS_RIGHT_DEG = 100;
static constexpr int COVER_PANEL_PROGRESS_SWEEP_DEG = 160;

// 外圈进度弧采样步进。
// 数值越小越细腻，但计算更多。
static constexpr int COVER_PANEL_PROGRESS_STEP_DEG = 1;

// 上方凸起圆心。
// 一般跟随 COVER_PANEL_Y。
static constexpr int COVER_PANEL_BUMP_CY = COVER_PANEL_Y + 1;
// 上方凸起半径。
static constexpr int COVER_PANEL_BUMP_R = 28;
// 中间唱片孔半径。
static constexpr int COVER_PANEL_HUB_R = 14;

// =============================================================================
// COVER_PANEL record overlay
// 封面前、面板后的一层半透明唱片圆。
// =============================================================================

// 唱片圆心：跟面板凸起中心一致。
// 这样面板盖住下半部分后，只露出上半圆。
static constexpr int COVER_PANEL_RECORD_CX = 120;
static constexpr int COVER_PANEL_RECORD_CY = COVER_PANEL_BUMP_CY;

// 唱片半径。
// 42~50 都可以，先用 47。
static constexpr int COVER_PANEL_RECORD_R = 40;// 唱片外圈半径，半透明混合

// 半透明强度。
// 0 完全透明，255 完全不透明。
// 100~130 比较合适。
static constexpr uint8_t COVER_PANEL_RECORD_ALPHA = 155;
static constexpr int COVER_PANEL_RECORD_INNER_R = 25;// 最内圈半径，不参与半透明混合


// 唱片扫描线半宽只与 abs(y - cy) 有关。
// 旧实现使用 240 个可写结构体，常驻占用 1920B 内部 RAM；
// 改为 67B Flash 只读表，并在绘制时恢复 x0/x1。
static constexpr uint8_t s_cover_panel_record_outer_half[41] = {
  40, 39, 39, 39, 39, 39, 39, 39, 39, 38, 38, 38, 38, 37, 37, 37,
  36, 36, 35, 35, 34, 34, 33, 32, 32, 31, 30, 29, 28, 27, 26, 25,
  24, 22, 21, 19, 17, 15, 12, 8, 0
};

static constexpr uint8_t s_cover_panel_record_inner_half[26] = {
  25, 24, 24, 24, 24, 24, 24, 24, 23, 23, 22, 22, 21, 21, 20, 20,
  19, 18, 17, 16, 15, 13, 11, 9, 7, 0
};

// 面板页 bilinear 旋转的源封面缓存。
// 安全版每个像素会 src->readPixel() 4 次，画质好但帧耗时高。
// 这里在切歌/换封面后只用 readPixel() 把 240x240 源封面缓存一次，
// 后续每帧旋转只访问普通 RGB565 数组，仍然用 drawPixel() 安全写回，避免上一版 getBuffer 写入花屏。
static uint16_t* s_cover_panel_src_cache = nullptr;
static LGFX_Sprite* s_cover_panel_src_cache_owner = nullptr;
static bool s_cover_panel_src_cache_valid = false;

void cover_panel_invalidate_source_cache()
{
  s_cover_panel_src_cache_valid = false;
  s_cover_panel_src_cache_owner = nullptr;
}

static bool cover_panel_ensure_source_cache(LGFX_Sprite* src)
{
  if (!src) return false;

  if (s_cover_panel_src_cache_valid && s_cover_panel_src_cache_owner == src && s_cover_panel_src_cache) {
    return true;
  }

  if (!s_cover_panel_src_cache) {
    const size_t bytes = (size_t)COVER_SIZE * (size_t)COVER_SIZE * sizeof(uint16_t);
    s_cover_panel_src_cache = (uint16_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_cover_panel_src_cache) {
      LOGW("[界面] COVER_PANEL 源封面缓存 PSRAM 分配失败，禁止回落内部RAM，回退 readPixel 采样 size=%lu",
           (unsigned long)bytes);
      return false;
    }
#if APP_DIAG_RAM_ATTRIBUTION
    LOGI("[内存归因] ui.cover_panel_src_cache ptr=%p bytes=%lu internal=%d psram=%d",
         s_cover_panel_src_cache,
         (unsigned long)bytes,
         esp_ptr_internal(s_cover_panel_src_cache) ? 1 : 0,
         esp_ptr_external_ram(s_cover_panel_src_cache) ? 1 : 0);
#endif
  }

  for (int y = 0; y < COVER_SIZE; ++y) {
    uint16_t* row = s_cover_panel_src_cache + y * COVER_SIZE;
    for (int x = 0; x < COVER_SIZE; ++x) {
      row[x] = (uint16_t)src->readPixel(x, y);
    }
  }

  s_cover_panel_src_cache_owner = src;
  s_cover_panel_src_cache_valid = true;
  return true;
}

static inline uint16_t cover_panel_cached_pixel(const uint16_t* cache, LGFX_Sprite* src, int x, int y)
{
  if (cache) {
    return cache[y * COVER_SIZE + x];
  }
  return (uint16_t)src->readPixel(x, y);
}

// RGB565 四点双线性混合。fx/fy 使用 10-bit 小数，范围 0..1023。
// 这里故意不直接操作 Sprite getBuffer()：上一版花屏大概率来自 Sprite 内部字节序/格式假设。
static uint16_t cover_bilinear_mix_rgb565(uint16_t c00,
                                          uint16_t c10,
                                          uint16_t c01,
                                          uint16_t c11,
                                          int fx,
                                          int fy)
{
  const int inv_fx = 1024 - fx;
  const int inv_fy = 1024 - fy;

  const int w00 = inv_fx * inv_fy;
  const int w10 = fx     * inv_fy;
  const int w01 = inv_fx * fy;
  const int w11 = fx     * fy;

  const int round = 1 << 19;

  const int r = (((int)((c00 >> 11) & 0x1F) * w00 +
                  (int)((c10 >> 11) & 0x1F) * w10 +
                  (int)((c01 >> 11) & 0x1F) * w01 +
                  (int)((c11 >> 11) & 0x1F) * w11 + round) >> 20);

  const int g = (((int)((c00 >> 5) & 0x3F) * w00 +
                  (int)((c10 >> 5) & 0x3F) * w10 +
                  (int)((c01 >> 5) & 0x3F) * w01 +
                  (int)((c11 >> 5) & 0x3F) * w11 + round) >> 20);

  const int b = (((int)(c00 & 0x1F) * w00 +
                  (int)(c10 & 0x1F) * w10 +
                  (int)(c01 & 0x1F) * w01 +
                  (int)(c11 & 0x1F) * w11 + round) >> 20);

  return (uint16_t)((r << 11) | (g << 5) | b);
}

static bool draw_fullscreen_rotated_cover_bilinear(LGFX_Sprite* dst, LGFX_Sprite* src, float angle_deg)
{
  const uint32_t prof_t0 = UI_PROF_T0();

  if (!dst || !src) return false;

  if (!cover_panel_ensure_source_cache(src) || !s_cover_panel_src_cache) {
    return false;
  }

  const uint16_t* src_cache = s_cover_panel_src_cache;

  const float rad = angle_deg * 0.01745329252f;
  const int c = (int)(cosf(rad) * 1024.0f);
  const int s = (int)(sinf(rad) * 1024.0f);

  static constexpr int FP = 10;
  static constexpr int ONE = 1 << FP;
  static constexpr int CENTER_FP = (COVER_SIZE / 2) << FP;
  const int max_fp = (COVER_SIZE - 2) << FP;

  // dst 已经在 cover_rotate_draw() 里 fillScreen(TFT_BLACK)。
  // 这里只绘制圆屏可见范围，减少约 20% 像素计算量，屏幕四角保持黑色。
  for (int y = 0; y < COVER_SIZE; ++y) {
    int x0 = 0;
    int w = 0;
    circle_span(y, 0, x0, w);
    if (w <= 0) continue;

    const int dy = y - COVER_SIZE / 2;
    const int x_end = x0 + w;
    const int dx0 = x0 - COVER_SIZE / 2;

    int sx_fp_row = CENTER_FP + dx0 * c + dy * s;
    int sy_fp_row = CENTER_FP - dx0 * s + dy * c;

    for (int x = x0; x < x_end; ++x) {
      int sx_fp = sx_fp_row;
      int sy_fp = sy_fp_row;
      sx_fp_row += c;
      sy_fp_row -= s;

      if (sx_fp < 0) sx_fp = 0;
      if (sy_fp < 0) sy_fp = 0;
      if (sx_fp > max_fp) sx_fp = max_fp;
      if (sy_fp > max_fp) sy_fp = max_fp;

      const int sx0 = sx_fp >> FP;
      const int sy0 = sy_fp >> FP;
      const int fx = sx_fp & (ONE - 1);
      const int fy = sy_fp & (ONE - 1);

      const int row0 = sy0 * COVER_SIZE;
      const int row1 = row0 + COVER_SIZE;
      const uint16_t c00 = src_cache[row0 + sx0];
      const uint16_t c10 = src_cache[row0 + sx0 + 1];
      const uint16_t c01 = src_cache[row1 + sx0];
      const uint16_t c11 = src_cache[row1 + sx0 + 1];

      const uint16_t color = cover_bilinear_mix_rgb565(c00, c10, c01, c11, fx, fy);
      dst->drawPixel(x, y, color);
    }
  }

  UI_PROF_SAMPLE(s_prof_fullscreen_bilinear, "fullscreen.bilinear", prof_t0);
  return true;
}

static void draw_upper_rotated_cover_sampled(LGFX_Sprite* dst, LGFX_Sprite* src, float angle_deg)
{
  if (!dst || !src) return;

  dst->fillScreen(TFT_BLACK);

  const bool cache_ok = cover_panel_ensure_source_cache(src);
  const uint16_t* src_cache = cache_ok ? s_cover_panel_src_cache : nullptr;

  const float rad = angle_deg * 0.01745329252f;
  const int c = (int)(cosf(rad) * 1024.0f);
  const int s = (int)(sinf(rad) * 1024.0f);

  static constexpr int FP = 10;
  static constexpr int ONE = 1 << FP;
  static constexpr int CENTER_FP = (COVER_SIZE / 2) << FP;

  for (int y = 0; y < COVER_PANEL_Y; ++y) {
    int x0 = 0;
    int w = 0;
    circle_span(y, 0, x0, w);
    if (w <= 0) continue;

    const int dy = y - COVER_SIZE / 2;
    const int x_end = x0 + w;
    const int dx0 = x0 - COVER_SIZE / 2;

    // 行内递增优化：同一行里 x 每增加 1，源坐标只需要固定增量。
    // 避免每个像素重复做 dx*c / dx*s 乘法，画质不变、风险低。
    int sx_fp_row = CENTER_FP + dx0 * c + dy * s;
    int sy_fp_row = CENTER_FP - dx0 * s + dy * c;
    const int max_fp = (COVER_SIZE - 2) << FP;

    for (int x = x0; x < x_end; ++x) {
      int sx_fp = sx_fp_row;
      int sy_fp = sy_fp_row;
      sx_fp_row += c;
      sy_fp_row -= s;

      // 为了安全读取 x+1/y+1，最大钳到 COVER_SIZE-2。
      if (sx_fp < 0) sx_fp = 0;
      if (sy_fp < 0) sy_fp = 0;
      if (sx_fp > max_fp) sx_fp = max_fp;
      if (sy_fp > max_fp) sy_fp = max_fp;

      const int sx0 = sx_fp >> FP;
      const int sy0 = sy_fp >> FP;
      const int fx = sx_fp & (ONE - 1);
      const int fy = sy_fp & (ONE - 1);

      const uint16_t c00 = cover_panel_cached_pixel(src_cache, src, sx0,     sy0);
      const uint16_t c10 = cover_panel_cached_pixel(src_cache, src, sx0 + 1, sy0);
      const uint16_t c01 = cover_panel_cached_pixel(src_cache, src, sx0,     sy0 + 1);
      const uint16_t c11 = cover_panel_cached_pixel(src_cache, src, sx0 + 1, sy0 + 1);

      const uint16_t color = cover_bilinear_mix_rgb565(c00, c10, c01, c11, fx, fy);
      dst->drawPixel(x, y, color);
    }
  }
}

static inline int div255_round_fast(int v)
{
  // 近似 round(v / 255)，避免每个颜色通道做整数除法。
  // v 的范围很小（RGB565 通道 * alpha），该写法用于 8-bit alpha blend 足够稳定。
  v += 128;
  return (v + (v >> 8)) >> 8;
}

static uint16_t blend_rgb565(uint16_t bg, uint16_t fg, uint8_t alpha)
{
  const uint8_t inv = 255 - alpha;

  const int br = (bg >> 11) & 0x1F;
  const int bgc = (bg >> 5) & 0x3F;
  const int bb = bg & 0x1F;

  const int fr = (fg >> 11) & 0x1F;
  const int fgc = (fg >> 5) & 0x3F;
  const int fb = fg & 0x1F;

  const int r = div255_round_fast(br * inv + fr * alpha);
  const int g = div255_round_fast(bgc * inv + fgc * alpha);
  const int b = div255_round_fast(bb * inv + fb * alpha);

  return (uint16_t)((r << 11) | (g << 5) | b);
}

static void fill_cover_panel_record_blend(LGFX_Sprite* dst, uint16_t color, uint8_t alpha)
{
  if (!dst || alpha == 0) return;

  int y0 = COVER_PANEL_RECORD_CY - COVER_PANEL_RECORD_R;
  int y1 = COVER_PANEL_RECORD_CY + COVER_PANEL_RECORD_R;
  if (y0 < 0) y0 = 0;
  if (y1 > 239) y1 = 239;

  for (int y = y0; y <= y1; ++y) {
    const int dy = y - COVER_PANEL_RECORD_CY;
    const int ady = (dy < 0) ? -dy : dy;
    const int outer_half = (int)s_cover_panel_record_outer_half[ady];
    const int x0 = COVER_PANEL_RECORD_CX - outer_half;
    const int x1 = COVER_PANEL_RECORD_CX + outer_half;

    int inner_x0 = 1;
    int inner_x1 = 0;
    if (ady <= COVER_PANEL_RECORD_INNER_R) {
      const int inner_half = (int)s_cover_panel_record_inner_half[ady];
      inner_x0 = COVER_PANEL_RECORD_CX - inner_half;
      inner_x1 = COVER_PANEL_RECORD_CX + inner_half;
    }

    for (int x = x0; x <= x1; ++x) {
      // 最内圈后面会被实心圆覆盖，这里直接跳过，避免无效 readPixel/blend/drawPixel。
      if (inner_x0 <= inner_x1 && x >= inner_x0 && x <= inner_x1) {
        x = inner_x1;
        continue;
      }

      const uint16_t bg = dst->readPixel(x, y);
      const uint16_t out = blend_rgb565(bg, color, alpha);
      dst->drawPixel(x, y, out);
    }
  }
}

static void draw_cover_panel_record_overlay(LGFX_Sprite* dst)
{
  if (!dst) return;

  const int cx = COVER_PANEL_RECORD_CX;
  const int cy = COVER_PANEL_RECORD_CY;
  const int r  = COVER_PANEL_RECORD_R;

  // 大唱片圆：半透明，但跳过最内圈。
  // 扫描线半宽已预计算，避免每帧 sqrtf 和 inner dx*dx 判断。
  fill_cover_panel_record_blend(dst, TFT_BLACK, COVER_PANEL_RECORD_ALPHA);

  // 内部弱环纹，保留一条
  dst->drawCircle(cx, cy, r - 7, 0x8430);

  // 最内圈：实心，不参与半透明混合
  const uint16_t inner_fill = 0x0841;

  dst->fillCircle(cx, cy, COVER_PANEL_RECORD_INNER_R, inner_fill);
}

static void draw_cover_panel_button(LGFX_Sprite* dst,
                                    int cx,
                                    int cy,
                                    int r,
                                    bool primary,
                                    bool active = false)
{
  if (!dst) return;

  uint16_t bg;
  uint16_t border;

  if (active) {
    bg = 0x07FF;        // 高亮青色
    border = TFT_WHITE;
  } else {
    bg = primary ? 0x30A4 : 0x18E3;
    border = primary ? UI_COLOR_BAR_CURSOR : 0x7BEF;
  }

  dst->fillCircle(cx, cy, r, bg);
  dst->drawCircle(cx, cy, r, border);
}

static void draw_cover_panel_play_pause(LGFX_Sprite* dst, int cx, int cy, uint16_t color)
{
  const bool paused = audio_service_is_paused() || !audio_service_is_playing();

  if (paused) {
    dst->fillTriangle(cx - 4, cy - 6, cx - 4, cy + 6, cx + 7, cy, color);
  } else {
    dst->fillRect(cx - 5, cy - 6, 4, 12, color);
    dst->fillRect(cx + 2, cy - 6, 4, 12, color);
  }
}

static void draw_cover_panel_prev_next(LGFX_Sprite* dst,
                                       int cx,
                                       int cy,
                                       bool next,
                                       uint16_t color)
{
  if (!dst) return;

  if (next) {
    dst->fillTriangle(cx - 5, cy - 4, cx - 5, cy + 4, cx,     cy, color);
    dst->fillTriangle(cx,     cy - 4, cx,     cy + 4, cx + 5, cy, color);
    dst->fillRect(cx + 6, cy - 5, 2, 10, color);
  } else {
    dst->fillTriangle(cx + 5, cy - 4, cx + 5, cy + 4, cx,     cy, color);
    dst->fillTriangle(cx,     cy - 4, cx,     cy + 4, cx - 5, cy, color);
    dst->fillRect(cx - 8, cy - 5, 2, 10, color);
  }
}

// 接近洋红也透明
static bool is_cover_panel_skin_transparent(uint16_t c)
{
  const int r = (c >> 11) & 0x1F;
  const int g = (c >> 5)  & 0x3F;
  const int b = c & 0x1F;

  if (c == COVER_PANEL_SKIN_KEY) {
    return true;
  }

  // 兼容顶部洋红背景的边缘过渡
  if (r >= 26 && b >= 26 && g <= 12 && abs(r - b) <= 5) {
    return true;
  }

  return false;
}

// 面板皮肤缓存。
// 原实现每帧都从 PROGMEM 读 240x140 像素、判断透明、再逐像素 drawPixel，
// 在封面面板页里会和 bilinear 旋转抢时间。这里把静态皮肤预渲染成 Sprite，
// 每帧只做一次带透明色的 pushSprite。
static LGFX_Sprite* s_cover_panel_skin_spr = nullptr;
static bool s_cover_panel_skin_spr_ready = false;
static bool s_cover_panel_skin_spr_failed = false;

static bool cover_panel_ensure_skin_sprite()
{
  if (s_cover_panel_skin_spr_ready && s_cover_panel_skin_spr) {
    return true;
  }
  if (s_cover_panel_skin_spr_failed) {
    return false;
  }

  if (!s_cover_panel_skin_spr) {
    s_cover_panel_skin_spr = new LGFX_Sprite(&tft);
    if (!s_cover_panel_skin_spr) {
      s_cover_panel_skin_spr_failed = true;
      return false;
    }
  }

  s_cover_panel_skin_spr->setColorDepth(16);
  s_cover_panel_skin_spr->setPsram(psramFound());
  if (!s_cover_panel_skin_spr->createSprite(COVER_PANEL_SKIN_W, COVER_PANEL_SKIN_H)) {
    LOGW("[界面] COVER_PANEL 皮肤缓存 Sprite 创建失败，回退逐像素绘制");
    s_cover_panel_skin_spr_failed = true;
    return false;
  }

  s_cover_panel_skin_spr->fillScreen(COVER_PANEL_SKIN_KEY);
  for (int y = 0; y < COVER_PANEL_SKIN_H; ++y) {
    for (int x = 0; x < COVER_PANEL_SKIN_W; ++x) {
      const uint16_t c = pgm_read_word(&g_cover_panel_skin_240x140[y * COVER_PANEL_SKIN_W + x]);
      if (is_cover_panel_skin_transparent(c)) {
        continue;
      }
      s_cover_panel_skin_spr->drawPixel(x, y, c);
    }
  }

  s_cover_panel_skin_spr_ready = true;
  return true;
}

static void draw_cover_panel_skin_fallback(LGFX_Sprite* dst)
{
  if (!dst) return;

  for (int y = 0; y < COVER_PANEL_SKIN_H; ++y) {
    const int sy = COVER_PANEL_SKIN_Y + y;
    if ((unsigned)sy >= 240) continue;

    for (int x = 0; x < COVER_PANEL_SKIN_W; ++x) {
      uint16_t c = pgm_read_word(&g_cover_panel_skin_240x140[y * COVER_PANEL_SKIN_W + x]);

      if (is_cover_panel_skin_transparent(c)) {
        continue;
      }

      dst->drawPixel(x, sy, c);
    }
  }
}

static void draw_cover_panel_skin(LGFX_Sprite* dst)
{
  if (!dst) return;

  if (cover_panel_ensure_skin_sprite()) {
    s_cover_panel_skin_spr->pushSprite(dst, 0, COVER_PANEL_SKIN_Y, COVER_PANEL_SKIN_KEY);
    return;
  }

  draw_cover_panel_skin_fallback(dst);
}

// COVER_PANEL title scroll state
static int s_cover_panel_title_scroll_x = 0;
static uint32_t s_cover_panel_title_scroll_last_ms = 0;

static const String& cover_panel_display_title()
{
  static String cached;
  static String last_title;
  static String last_artist;

  if (last_title != s_np_title || last_artist != s_np_artist) {
    last_title = s_np_title;
    last_artist = s_np_artist;

    s_cover_panel_title_scroll_x = 0;
    s_cover_panel_title_scroll_last_ms = 0;

    if (s_np_title.length() == 0) {
      cached = "";
    } else if (s_np_artist.length() == 0) {
      cached = s_np_title;
    } else if (s_np_title.indexOf(s_np_artist) >= 0) {
      cached = s_np_title;
    } else {
      cached = s_np_artist + " - " + s_np_title;
    }
  }

  return cached;
}

static bool cover_panel_text_span(int y, int pad, int& x0, int& w)
{
  static constexpr int CX = 120;
  static constexpr int CY = 120;

  const int R = COVER_PANEL_INNER_R;  // 这里用 114，给外圈进度弧留空间

  const int dy = y - CY;
  if (dy < -R || dy > R) {
    x0 = 0;
    w = 0;
    return false;
  }

  const int half = (int)sqrtf((float)(R * R - dy * dy));

  int left  = CX - half + pad;
  int right = CX + half - pad;

  if (left < 0) left = 0;
  if (right > 239) right = 239;

  if (right <= left) {
    x0 = 0;
    w = 0;
    return false;
  }

  x0 = left;
  w = right - left + 1;
  return true;
}

static void draw_cover_panel_title(LGFX_Sprite* dst, int y, uint16_t color)
{
  if (!dst) return;

  const String& title = cover_panel_display_title();
  if (title.length() == 0) return;

  dst->setFont(&g_font_cjk);
  dst->setTextSize(1);
  dst->setTextWrap(false);

  int x0 = 0;
  int available_w = 0;
  if (!cover_panel_text_span(y, COVER_PANEL_TEXT_SAFE_PAD, x0, available_w)) {
    return;
  }

  const int text_w = dst->textWidth(title.c_str());

  if (text_w <= available_w) {
    const int tx = x0 + (available_w - text_w) / 2;

    dst->setClipRect(x0, y - 11, available_w, 26);
    dst->setTextColor(color);
    dst->setCursor(tx, y);
    dst->print(title);
    dst->clearClipRect();

    s_cover_panel_title_scroll_x = 0;
    s_cover_panel_title_scroll_last_ms = 0;
    return;
  }

  uint32_t now = millis();

  if (s_cover_panel_title_scroll_last_ms == 0) {
    s_cover_panel_title_scroll_last_ms = now;
  }

  if (now - s_cover_panel_title_scroll_last_ms > 30) {
    s_cover_panel_title_scroll_last_ms = now;
    s_cover_panel_title_scroll_x += SCROLL_SPEED;
  }

  if (s_cover_panel_title_scroll_x > text_w + SCROLL_GAP) {
    s_cover_panel_title_scroll_x = 0;
  }

  dst->setClipRect(x0, y - 11, available_w, 26);
  dst->setTextColor(color);

  const int x1 = x0 - s_cover_panel_title_scroll_x;
  dst->setCursor(x1, y);
  dst->print(title);

  const int x2 = x1 + text_w + SCROLL_GAP;
  dst->setCursor(x2, y);
  dst->print(title);

  dst->clearClipRect();
}

/**
 * 基于时间的推移式滚动歌词
 * @param progress: 当前句播放进度 (0.0 到 1.0)
 */
static void draw_cover_panel_scrolling_line_by_time(LGFX_Sprite* dst,
                                                   const char* text,
                                                   int y,
                                                   int safe_pad,
                                                   uint16_t color,
                                                   float progress)
{
  if (!dst || !text || text[0] == '\0') return;

  dst->setTextColor(color);
  dst->setFont(&g_font_cjk);
  dst->setTextSize(1);
  dst->setTextWrap(false);

  int x0 = 0;
  int available_w = 0;
    if (!cover_panel_text_span(y, safe_pad, x0, available_w)) {
    return;
  }

  if (available_w <= 20) return;

  const int text_w = dst->textWidth(text);

  // 给边界留一点余量：
  // 只要接近放不下，就走滚动，避免只差一个字时显示成 ...
  static constexpr int SCROLL_TRIGGER_MARGIN = 6;

  if (text_w <= available_w - SCROLL_TRIGGER_MARGIN) {
    // 这里不要调用 draw_center_text_on_sprite()
    // 避免它内部再次裁剪成 ...
    const int tx = x0 + (available_w - text_w) / 2;

    dst->setClipRect(x0, y - 11, available_w, 26);
    dst->setCursor(tx, y);
    dst->print(text);
    dst->clearClipRect();
    return;
  }

  int max_scroll = text_w - available_w;

  // 如果只是非常接近边界，比如只差几个像素，
  // 也给它一个最小滚动量，避免看起来完全不动。
  if (max_scroll < 1) {
    max_scroll = 1;
  }

  float scroll_factor = 0.0f;

  // 前 10% 停在开头，中间 80% 根据歌词时间滚动，最后 10% 停在结尾
  if (progress < 0.1f) {
    scroll_factor = 0.0f;
  } else if (progress > 0.9f) {
    scroll_factor = 1.0f;
  } else {
    scroll_factor = (progress - 0.1f) / 0.8f;
  }

  if (scroll_factor < 0.0f) scroll_factor = 0.0f;
  if (scroll_factor > 1.0f) scroll_factor = 1.0f;

  const int current_offset = (int)(max_scroll * scroll_factor);

  dst->setClipRect(x0, y - 11, available_w, 26);
  dst->setCursor(x0 - current_offset, y);
  dst->print(text);
  dst->clearClipRect();
}

static void draw_scrolling_line_by_time(LGFX_Sprite* dst,
                                        const char* text,
                                        int y,
                                        int safe_pad,
                                        uint16_t color,
                                        float progress)
{
  if (!dst || !text || text[0] == '\0') return;

  dst->setTextColor(color);
  dst->setFont(&g_font_cjk);
  dst->setTextSize(1);
  dst->setTextWrap(false);

  const int available_w = 240 - safe_pad * 2;
  const int text_w = dst->textWidth(text);

  if (text_w <= available_w) {
    draw_center_text_on_sprite(dst, text, y, color, safe_pad);
    return;
  }

  const int max_scroll = text_w - available_w;

  float scroll_factor = 0.0f;

  if (progress < 0.1f) {
    scroll_factor = 0.0f;
  } else if (progress > 0.9f) {
    scroll_factor = 1.0f;
  } else {
    scroll_factor = (progress - 0.1f) / 0.8f;
  }

  if (scroll_factor < 0.0f) scroll_factor = 0.0f;
  if (scroll_factor > 1.0f) scroll_factor = 1.0f;

  const int current_offset = (int)(max_scroll * scroll_factor);

  dst->setClipRect(safe_pad, y - 11, available_w, 26);
  dst->setCursor(safe_pad - current_offset, y);
  dst->print(text);
  dst->clearClipRect();
}

static void draw_cover_panel_center_text(LGFX_Sprite* dst,
                                         const char* text,
                                         int y,
                                         uint16_t color,
                                         int pad)
{
  if (!dst || !text || text[0] == '\0') return;

  dst->setFont(&g_font_cjk);
  dst->setTextSize(1);
  dst->setTextWrap(false);

  int x0 = 0;
  int available_w = 0;
  if (!cover_panel_text_span(y, pad, x0, available_w)) {
    return;
  }

  String t = clip_utf8_by_px(dst, String(text), available_w);
  if (t.length() == 0) return;

  const int text_w = dst->textWidth(t.c_str());
  const int tx = x0 + (available_w - text_w) / 2;

  dst->setClipRect(x0, y - 11, available_w, 26);
  dst->setTextColor(color);
  dst->setCursor(tx, y);
  dst->print(t);
  dst->clearClipRect();
}

static void draw_cover_panel_info(LGFX_Sprite* dst)
{
  if (!dst) return;

  const uint16_t c_text        = UI_COLOR_TIME;
  const uint16_t c_title       = 0xFFE0;
  const uint16_t c_lyrics      = 0xFFFF;
  const uint16_t c_lyrics_next = 0x8C71;

  // ============================================================
  // 1. 第一行：音量 + 播放按钮 + 模式
  // ============================================================
  const int y_play = COVER_PANEL_PLAY_Y;
  const int y_side = COVER_PANEL_SIDE_Y;

  const uint32_t now = millis();
  ui_lock();
  const int8_t nav_feedback = s_cover_panel_nav_feedback;
  const uint32_t nav_feedback_until_ms = s_cover_panel_nav_feedback_until_ms;
  ui_unlock();

  const bool feedback_alive =
      static_cast<int32_t>(nav_feedback_until_ms - now) > 0;

  const bool prev_active = feedback_alive && (nav_feedback < 0);
  const bool next_active = feedback_alive && (nav_feedback > 0);

  draw_cover_panel_button(dst,
                          COVER_PANEL_PREV_X,
                          y_side,
                          COVER_PANEL_SIDE_R,
                          false,
                          prev_active);

  draw_cover_panel_button(dst,
                          COVER_PANEL_NEXT_X,
                          y_side,
                          COVER_PANEL_SIDE_R,
                          false,
                          next_active);

  // 播放按钮底圈由皮肤图提供，这里不再画圆形按钮
  // draw_cover_panel_button(dst,
  //                         COVER_PANEL_PLAY_X,
  //                         y_play,
  //                         COVER_PANEL_PLAY_R,
  //                         true,
  //                         false);

  draw_cover_panel_prev_next(dst,
                            COVER_PANEL_PREV_X,
                            y_side,
                            false,
                            prev_active ? TFT_BLACK : TFT_WHITE);

  draw_cover_panel_play_pause(dst,
                              COVER_PANEL_PLAY_X,
                              y_play,
                              TFT_WHITE);

  draw_cover_panel_prev_next(dst,
                            COVER_PANEL_NEXT_X,
                            y_side,
                            true,
                            next_active ? TFT_BLACK : TFT_WHITE);

  // 音量 / 模式只和小按钮居中对齐
  draw_cover_panel_status_icons(dst, COVER_PANEL_STATUS_Y, c_text);

  // ============================================================
  // 2. 第二行：时间
  // ============================================================
  const uint32_t play_ms  = audio_get_play_ms();
  const uint32_t total_ms = audio_get_total_ms();

  char time_buf[32];
  snprintf(time_buf,
           sizeof(time_buf),
           "%02lu:%02lu  |  %02lu:%02lu",
           (unsigned long)(play_ms / 60000UL),
           (unsigned long)((play_ms / 1000UL) % 60UL),
           (unsigned long)(total_ms / 60000UL),
           (unsigned long)((total_ms / 1000UL) % 60UL));

  draw_center_text_on_sprite(dst, time_buf, COVER_PANEL_TIME_Y, c_text, COVER_PANEL_SAFE_PAD);

  // ============================================================
  // 3. 第三行：歌名
  // ============================================================
  draw_cover_panel_title(dst, COVER_PANEL_TITLE_Y, c_title);

  // ============================================================
  // 4. 第四、五行：歌词
  // ============================================================
  if (g_lyricsDisplay.hasLyrics()) {
    LyricsDisplay::ScrollLyrics scroll = g_lyricsDisplay.getScrollLyrics();

    if (scroll.current && scroll.current[0] != '\0') {
      draw_cover_panel_scrolling_line_by_time(dst,
                                              scroll.current,
                                              COVER_PANEL_LYRIC_CUR_Y,
                                              COVER_PANEL_TEXT_SAFE_PAD,
                                              c_lyrics,
                                              scroll.progress);
    }

    if (scroll.next && scroll.next[0] != '\0') {
      draw_cover_panel_center_text(dst,
                                  scroll.next,
                                  COVER_PANEL_LYRIC_NEXT_Y,
                                  c_lyrics_next,
                                  COVER_PANEL_TEXT_SAFE_PAD);
    }
  }
}

// 旧实现首次绘制时生成 360 个可写三角函数点，常驻占用 1440B 内部 RAM。
// 整数角度具备四象限对称性，只保留 0~90 度正弦值到 Flash。
static constexpr int16_t s_cover_panel_sin1024_quarter[91] = {
  0, 18, 36, 54, 71, 89, 107, 125, 143, 160, 178, 195,
  213, 230, 248, 265, 282, 299, 316, 333, 350, 367, 384, 400,
  416, 433, 449, 465, 481, 496, 512, 527, 543, 558, 573, 587,
  602, 616, 630, 644, 658, 672, 685, 698, 711, 724, 737, 749,
  761, 773, 784, 796, 807, 818, 828, 839, 849, 859, 868, 878,
  887, 896, 904, 912, 920, 928, 935, 943, 949, 956, 962, 968,
  974, 979, 984, 989, 994, 998, 1002, 1005, 1008, 1011, 1014, 1016,
  1018, 1020, 1022, 1023, 1023, 1024, 1024
};

static int cover_panel_norm_deg(int deg)
{
  deg %= 360;
  if (deg < 0) deg += 360;
  return deg;
}

static inline int cover_panel_sin1024(int deg)
{
  deg = cover_panel_norm_deg(deg);
  if (deg <= 90) {
    return (int)s_cover_panel_sin1024_quarter[deg];
  }
  if (deg <= 180) {
    return (int)s_cover_panel_sin1024_quarter[180 - deg];
  }
  if (deg <= 270) {
    return -(int)s_cover_panel_sin1024_quarter[deg - 180];
  }
  return -(int)s_cover_panel_sin1024_quarter[360 - deg];
}

static inline int cover_panel_arc_x(int cx, int radius, int deg)
{
  // 原 cos(deg - 90°) 等价于 sin(deg)。
  const int v = cover_panel_sin1024(deg) * radius;
  return cx + ((v >= 0) ? ((v + 512) >> 10) : -(((-v) + 512) >> 10));
}

static inline int cover_panel_arc_y(int cy, int radius, int deg)
{
  // 原 sin(deg - 90°) 直接用同一张四分之一正弦表。
  const int v = cover_panel_sin1024(deg - 90) * radius;
  return cy + ((v >= 0) ? ((v + 512) >> 10) : -(((-v) + 512) >> 10));
}

static uint16_t lerp_rgb565_int(uint16_t c0, uint16_t c1, int num, int den)
{
  if (den <= 0) return c0;
  if (num < 0) num = 0;
  if (num > den) num = den;

  const int r0 = (c0 >> 11) & 0x1F;
  const int g0 = (c0 >> 5)  & 0x3F;
  const int b0 = c0 & 0x1F;

  const int r1 = (c1 >> 11) & 0x1F;
  const int g1 = (c1 >> 5)  & 0x3F;
  const int b1 = c1 & 0x1F;

  const int r = r0 + ((r1 - r0) * num + den / 2) / den;
  const int g = g0 + ((g1 - g0) * num + den / 2) / den;
  const int b = b0 + ((b1 - b0) * num + den / 2) / den;

  return (uint16_t)((r << 11) | (g << 5) | b);
}

static void draw_cover_panel_arc(LGFX_Sprite* dst,
                                 int cx,
                                 int cy,
                                 int r,
                                 int start_deg,
                                 int end_deg,
                                 int thickness,
                                 uint16_t color)
{
  if (!dst) return;
  if (start_deg == end_deg) return;

  const int step = (end_deg >= start_deg)
      ? COVER_PANEL_PROGRESS_STEP_DEG
      : -COVER_PANEL_PROGRESS_STEP_DEG;

  for (int t = 0; t < thickness; ++t) {
    const int rr = r - t;

    bool has_prev = false;
    int prev_x = 0;
    int prev_y = 0;

    for (int deg = start_deg;
         (step > 0) ? (deg <= end_deg) : (deg >= end_deg);
         deg += step) {
      const float rad = (deg - 90) * 0.01745329252f;

      const int x = cx + (int)(cosf(rad) * rr);
      const int y = cy + (int)(sinf(rad) * rr);

      if (has_prev) {
        dst->drawLine(prev_x, prev_y, x, y, color);

        // 补点，减少整数取样造成的小断裂
        dst->drawPixel(x, y, color);
        dst->drawPixel(prev_x, prev_y, color);
      } else {
        dst->drawPixel(x, y, color);
      }

      prev_x = x;
      prev_y = y;
      has_prev = true;
    }
  }
}

static void draw_cover_panel_arc_gradient(LGFX_Sprite* dst,
                                          int cx,
                                          int cy,
                                          int r,
                                          int start_deg,
                                          int end_deg,
                                          int thickness,
                                          uint16_t left_color,
                                          uint16_t right_color,
                                          int full_left_deg,
                                          int full_right_deg)
{
  if (!dst) return;
  if (start_deg == end_deg) return;

  const int step = (end_deg >= start_deg)
      ? COVER_PANEL_PROGRESS_STEP_DEG
      : -COVER_PANEL_PROGRESS_STEP_DEG;

  const int full_range = abs(full_left_deg - full_right_deg);
  if (full_range <= 0) return;

  for (int line = 0; line < thickness; ++line) {
    const int rr = r - line;

    bool has_prev = false;
    int prev_x = 0;
    int prev_y = 0;

    for (int deg = start_deg;
         (step > 0) ? (deg <= end_deg) : (deg >= end_deg);
         deg += step) {
      const float rad = (deg - 90) * 0.01745329252f;

      const int x = cx + (int)(cosf(rad) * rr);
      const int y = cy + (int)(sinf(rad) * rr);

      // 这里按"左 -> 右"算渐变比例：
      // deg = full_left_deg  时 t=0，蓝色
      // deg = full_right_deg 时 t=1，粉色
      const int num = (full_left_deg > full_right_deg)
          ? (full_left_deg - deg)
          : (deg - full_left_deg);
      const uint16_t color = lerp_rgb565_int(left_color, right_color, num, full_range);

      if (has_prev) {
        dst->drawLine(prev_x, prev_y, x, y, color);
      } else {
        dst->drawPixel(x, y, color);
      }

      prev_x = x;
      prev_y = y;
      has_prev = true;
    }
  }
}

static void draw_cover_panel_progress_ring(LGFX_Sprite* dst)
{
  if (!dst) return;

  const uint32_t play_ms  = audio_get_play_ms();
  const uint32_t total_ms = audio_get_total_ms();

  float progress = 0.0f;
  if (total_ms > 0) {
    progress = (float)play_ms / (float)total_ms;
  }

  if (progress < 0.0f) progress = 0.0f;
  if (progress > 1.0f) progress = 1.0f;

  const int cx = 120;
  const int cy = 120;
  const int r  = COVER_PANEL_PROGRESS_R;

  // 左下 -> 右下
  const int left_deg  = COVER_PANEL_PROGRESS_LEFT_DEG;
  const int right_deg = COVER_PANEL_PROGRESS_RIGHT_DEG;

  // progress = 0 -> 左侧
  // progress = 1 -> 右侧
  const float played_deg_f =
      left_deg - progress * (float)COVER_PANEL_PROGRESS_SWEEP_DEG;

  const int played_deg = (int)played_deg_f;

  // 暗色底弧
  // const uint16_t bg_color = 0x2945;

  // 左蓝右粉
  const uint16_t left_color  = 0x07FF;  // cyan / blue
  const uint16_t right_color = 0xF81F;  // pink / magenta

  // ============================================================
  // 1. 底部背景弧：只画面板下方区域（由皮肤纹理显示）
  // ============================================================
  // draw_cover_panel_arc(dst,
  //                      cx,
  //                      cy,
  //                      r,
  //                      left_deg,
  //                      right_deg,
  //                      COVER_PANEL_PROGRESS_THICKNESS,
  //                      bg_color);

  if (total_ms == 0) {
    return;
  }

  // ============================================================
  // 2. 已播放进度弧：左蓝 -> 右粉渐变
  // ============================================================
  draw_cover_panel_arc_gradient(dst,
                                cx,
                                cy,
                                r,
                                left_deg,
                                played_deg,
                                COVER_PANEL_PROGRESS_THICKNESS,
                                left_color,
                                right_color,
                                left_deg,
                                right_deg);

  // ============================================================
  // 3. 当前播放位置圆点：白点黑心
  // ============================================================
  {
    const float rad = (played_deg_f - 90.0f) * 0.01745329252f;

    // 圆点走在弧线厚度中心，不走最外侧
    const int dot_r_path = r - (COVER_PANEL_PROGRESS_THICKNESS / 2);

    const int x = cx + (int)roundf(cosf(rad) * dot_r_path);
    const int y = cy + (int)roundf(sinf(rad) * dot_r_path);

    dst->fillCircle(x, y, 3, TFT_WHITE);
    dst->fillCircle(x, y, 1, TFT_BLACK);
  }
}


void cover_panel_draw(float angle_deg)
{
  const uint32_t prof_total_t0 = UI_PROF_T0();

  if (!s_rotFramesInited || !s_src) return;

  auto* dst = s_rotFrame[s_rotBack];

  // 1. 只画上半圆旋转封面
  uint32_t prof_step_t0 = UI_PROF_T0();
  draw_upper_rotated_cover_sampled(dst, s_src, angle_deg);
  UI_PROF_SAMPLE(s_prof_panel_cover, "panel.cover", prof_step_t0);

  // 2. 半透明唱片圆：封面前、面板后
  prof_step_t0 = UI_PROF_T0();
  draw_cover_panel_record_overlay(dst);
  UI_PROF_SAMPLE(s_prof_panel_record, "panel.record", prof_step_t0);

  // 3. 下半固定面板覆盖
  prof_step_t0 = UI_PROF_T0();
  draw_cover_panel_skin(dst);
  UI_PROF_SAMPLE(s_prof_panel_skin, "panel.skin", prof_step_t0);

  // 4. 面板文字和按钮
  prof_step_t0 = UI_PROF_T0();
  draw_cover_panel_info(dst);
  UI_PROF_SAMPLE(s_prof_panel_info, "panel.info", prof_step_t0);

  // 5. 最后画外圈进度弧，避免被面板覆盖
  prof_step_t0 = UI_PROF_T0();
  draw_cover_panel_progress_ring(dst);
  UI_PROF_SAMPLE(s_prof_panel_progress, "panel.progress", prof_step_t0);

  // 6. 电池状态页脚
  ui_draw_battery_footer(dst);

  // 7. 睡眠关机倒计时
  draw_sleep_timer_overlay(dst);

  // 8. 低电弹窗
  draw_low_battery_hint_overlay(dst);

  // 9. RTC 闹钟开机后的临时提示
  draw_alarm_wakeup_popup_overlay(dst);

  // 10. 绘制音量步进小提示 / 快退快进预览
  draw_volume_step_hint_overlay(dst);
  draw_seek_preview_overlay(dst);
  draw_nfc_bind_target_popup_overlay(dst);
  draw_nfc_notice_popup_overlay(dst);
  draw_nfc_scan_popup_overlay(dst);

  prof_step_t0 = UI_PROF_T0();
  dst->pushSprite(0, 0);
  UI_PROF_SAMPLE(s_prof_panel_push, "panel.push", prof_step_t0);
  UI_PROF_SAMPLE(s_prof_panel_total, "panel.total", prof_total_t0);

  uint8_t tmp = s_rotFront;
  s_rotFront = s_rotBack;
  s_rotBack = tmp;
}

void cover_info_draw()
{
  if (!s_framesInited) return;

  uint32_t t0 = millis();

  auto* dst = s_frame[s_back];
  dst->fillScreen(TFT_BLACK);

  uint32_t t1 = millis();

  // 1) 静态封面（整屏）- 使用带遮罩的版本
  if (s_coverSprReady) {
    s_coverMasked.pushSprite(dst, 0, 0);
  }
  uint32_t t_cover = millis();

  // 2) 参数：圆屏安全边距 + 更大的字号 + 更紧凑的排布
  const int safe_pad = 12;

  const uint16_t c_title  = 0xFFE0;   // 歌名文字颜色（纯白）
  const uint16_t c_artist = UI_COLOR_ARTIST;  // 歌手文字颜色（浅灰）
  const uint16_t c_lyrics = 0xFFFF;           // 歌词颜色（白色）
  const uint16_t c_lyrics_next = 0xAD55;      // 下一句歌词颜色（亮灰）

  // 3) 把信息区抬高一点，避免圆屏底部变窄导致左右被裁

  const int y_status = 131;  // 状态栏（音量/模式/列表）上移1像素
  const int y_bar   = 149;   // 进度条下移1像素
  const int y_time  = 157;   // 时间（上移3像素）
  const int y_title = 176;   // 标题
  const int y_artist= 195;   // 歌手（下移3像素）

  // 4) 歌词/信息页不再显示 WiFi 信息。
  // WiFi 状态已放在菜单中查看，这里优先留给闹钟状态和歌词区域。

  // 5) 歌词显示（屏幕上半部分）- 在遮罩之后绘制，确保可见
  bool hasLyrics = g_lyricsDisplay.hasLyrics();
  #if UI_LYRICS_STATE_DEBUG_LOG
  {
    static bool s_last_has_lyrics = false;
    static bool s_last_has_lyrics_valid = false;
    if (!s_last_has_lyrics_valid || s_last_has_lyrics != hasLyrics) {
      LOGD("[界面] hasLyrics: %d", hasLyrics);
      s_last_has_lyrics = hasLyrics;
      s_last_has_lyrics_valid = true;
    }
  }
#endif
  if (hasLyrics) {
    // 使用滚动歌词显示（3行：上一句、当前、下一句）
    LyricsDisplay::ScrollLyrics scroll = g_lyricsDisplay.getScrollLyrics();
    
    // 歌词显示位置 - 使用 constexpr 便于编译器优化
    static constexpr int Y_LYRICS_CENTER = 93;   // 中心位置
    static constexpr int LINE_HEIGHT = 20;       // 行高
    static constexpr int ANIM_END_PERCENT = 80;  // 前80%完成动画
    
    // 预计算缓动表（0-100 映射到 0-100 的 ease-out 曲线）
    // ease-out: y = 1 - (1-x)^2，展开后避免 pow 调用
    static const uint8_t ease_table[101] = {
      0,  2,  4,  6,  8, 10, 12, 14, 15, 17, 19, 21, 23, 24, 26, 28,
     30, 31, 33, 35, 36, 38, 40, 41, 43, 44, 46, 47, 49, 50, 52, 53,
     55, 56, 58, 59, 60, 62, 63, 64, 66, 67, 68, 70, 71, 72, 73, 75,
     76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91,
     91, 92, 93, 94, 94, 95, 96, 96, 97, 97, 98, 98, 99, 99, 99,100,
    100,100,100,100,100,100,100,100,100,100,100,100,100,100,100,100,
    100,100,100,100,100
    };
    
    // 将进度转换为 0-100 的整数索引
    int progress_int = (int)(scroll.progress * 100.0f);
    if (progress_int > 100) progress_int = 100;
    if (progress_int < 0) progress_int = 0;
    
    // 计算动画进度（前80%完成动画）
    int anim_progress_int;
    if (progress_int < ANIM_END_PERCENT) {
      anim_progress_int = (progress_int * 100) / ANIM_END_PERCENT;
    } else {
      anim_progress_int = 100;
    }
    // 安全检查：确保不越界
    if (anim_progress_int > 100) anim_progress_int = 100;
    if (anim_progress_int < 0) anim_progress_int = 0;

    // 查表获取缓动值，计算偏移（避免浮点运算）
    int offset = (ease_table[anim_progress_int] * LINE_HEIGHT) / 100;
    
    // 绘制上一句（灰色，淡出效果）
    if (scroll.prev && scroll.prev[0] != '\0') {
      draw_center_text_on_sprite(dst, scroll.prev, 
                                Y_LYRICS_CENTER - LINE_HEIGHT - offset, 
                                c_lyrics_next, safe_pad);
    }
    
    // 绘制当前句（白色，高亮，支持基于时间的滚动）
    if (scroll.current && scroll.current[0] != '\0') {
      draw_scrolling_line_by_time(dst, scroll.current, 
                                Y_LYRICS_CENTER - offset, 
                                safe_pad, c_lyrics, scroll.progress);
    }
    
    // 绘制下一句（灰色，淡入效果）
    if (scroll.next && scroll.next[0] != '\0') {
      draw_center_text_on_sprite(dst, scroll.next, 
                                Y_LYRICS_CENTER + LINE_HEIGHT - offset, 
                                c_lyrics_next, safe_pad);
    }
  }

  uint32_t t_lyrics = millis();

  // 时间显示颜色常量
  const uint16_t c_time_text = UI_COLOR_TIME;  // 时间文字颜色（浅灰）

  // 4) 进度条（显示 elapsed + total）
  uint32_t el_ms = audio_get_play_ms();
  uint32_t total_ms = audio_get_total_ms();

  char total_str[6];
  if (total_ms >= 1000 && total_ms != 0xFFFFFFFFu) {
    fmt_mmss(total_ms, total_str);
  } else {
    memcpy(total_str, "--:--", 6); // 包含 '\0'
  }

  // 检查音量是否处于激活状态。
  const UiPlayerRuntimeSnapshot runtime = ui_player_runtime_snapshot_get();
  bool volume_active = (millis() - runtime.volume_active_time) < VOLUME_ACTIVE_TIMEOUT_MS;
  draw_status_row(dst, y_status, safe_pad, c_time_text, volume_active);

  draw_time_bar(dst,
                y_bar, y_time,
                el_ms,
                total_ms,
                safe_pad,
                c_time_text);

  uint32_t t_status = millis();

  // 5) 标题/歌手（支持滚动显示长文本）
    
  // 更新滚动偏移（像素滚动，30ms间隔）
  uint32_t now = millis();
  if (now - s_scroll_last_ms > 30) {
    s_scroll_last_ms = now;
    
    // 标题滚动
    bool title_scroll = draw_scrolling_text_with_icon(dst, y_title, s_np_title, s_title_scroll_x, 
                                                       14, c_title, safe_pad, draw_note_icon_img);
    if (title_scroll) {
      s_title_scroll_x += SCROLL_SPEED;
      // 滚动范围：文本宽度 + 间距
      dst->setFont(&g_font_cjk);
      int title_w = dst->textWidth(s_np_title.c_str());
      if (s_title_scroll_x > title_w + SCROLL_GAP) {
        s_title_scroll_x = 0;
      }
    } else {
      s_title_scroll_x = 0;
    }
    
    // 歌手滚动
    bool artist_scroll = draw_scrolling_text_with_icon(dst, y_artist, s_np_artist, s_artist_scroll_x,
                                                        14, c_artist, safe_pad, draw_artist_icon_img);
    if (artist_scroll) {
      s_artist_scroll_x += SCROLL_SPEED;
      dst->setFont(&g_font_cjk);
      int artist_w = dst->textWidth(s_np_artist.c_str());
      if (s_artist_scroll_x > artist_w + SCROLL_GAP) {
        s_artist_scroll_x = 0;
      }
    } else {
      s_artist_scroll_x = 0;
    }
  } else {
    // 使用当前偏移绘制
    draw_scrolling_text_with_icon(dst, y_title, s_np_title, s_title_scroll_x, 
                                  14, c_title, safe_pad, draw_note_icon_img);
    draw_scrolling_text_with_icon(dst, y_artist, s_np_artist, s_artist_scroll_x,
                                  14, c_artist, safe_pad, draw_artist_icon_img);
  }

  uint32_t t_text = millis();
  // 6) 电池状态页脚
  ui_draw_battery_footer(dst);

  // 7) 睡眠关机倒计时 Overlay
  draw_sleep_timer_overlay(dst);

  // 8) 歌词/信息页显示闹钟状态。
  // 位置由 draw_alarm_status_overlay() 根据圆屏安全区和睡眠倒计时自动计算。
  draw_alarm_status_overlay(dst);

  // 9) 低电弹窗 Overlay
  draw_low_battery_hint_overlay(dst);

  // 10) RTC 闹钟开机后的临时提示 Overlay
  draw_alarm_wakeup_popup_overlay(dst);

  // 11) 音量步进小提示 / 快退快进预览 Overlay
  draw_volume_step_hint_overlay(dst);
  draw_seek_preview_overlay(dst);
  draw_nfc_bind_target_popup_overlay(dst);
  draw_nfc_notice_popup_overlay(dst);
  draw_nfc_scan_popup_overlay(dst);

  // 8) 推屏
  dst->pushSprite(0, 0);

  uint32_t t_push = millis();

  uint8_t tmp = s_front;
  s_front = s_back;
  s_back  = tmp;
}

bool ui_draw_cover_for_track(const TrackInfo& t, bool force_redraw)
{
  static String last_sig;
  String sig = t.audio_path + "#" + String((unsigned)t.cover_offset) + "#" + String((unsigned)t.cover_size);
  if (!force_redraw && sig == last_sig) return true;
  last_sig = sig;

  bool ok = cover_decode_to_sprite_from_track(t);
  if (!ok) return false;

  ui_draw_lock();
  cover_set_source(&s_coverSpr);
  s_angle_deg = 0.0f;
  s_rot_last_ms = 0;   // 让 UiTask 下次自己初始化 dt

  // 立即推送第一帧。
  // 如果此时有 NFC 弹窗，必须走当前播放器视图的完整绘制路径，
  // 让弹窗作为 overlay 一起画进去，避免直接推封面把弹窗短暂盖掉。
  if (!s_screen_cleared) {
    tft.fillScreen(TFT_BLACK);
    s_screen_cleared = true;
  }

  const bool nfc_overlay_visible =
      ui_nfc_bind_target_popup_is_visible() ||
      ui_nfc_notice_popup_is_visible() ||
      ui_nfc_scan_popup_is_visible();
  if (nfc_overlay_visible) {
    const ui_player_view_t view = ui_get_view();
    if (view == UI_VIEW_COVER_PANEL) {
      cover_panel_draw(0.0f);
    } else if (view == UI_VIEW_INFO) {
      cover_info_draw();
    } else {
      cover_rotate_draw(0.0f);
    }
  } else {
    s_coverSpr.pushSprite(0, 0);
  }

  s_angle_deg = 0.0f;
  s_rot_last_ms = millis();
  ui_draw_unlock();
  ui_request_refresh();

  return true;
}

void ui_set_now_playing(const char* title, const char* artist)
{
  String normalized_title = title ? String(title) : String("");
  String normalized_artist = artist ? String(artist) : String("");
  const uint32_t title_changes =
      text_normalize_display_spaces_inplace(normalized_title);
  const uint32_t artist_changes =
      text_normalize_display_spaces_inplace(normalized_artist);
  normalized_title.trim();
  normalized_artist.trim();
  if (title_changes + artist_changes > 0u) {
    LOGD("[文本] 已规范化播放信息空白：标题=%lu 歌手=%lu",
         (unsigned long)title_changes,
         (unsigned long)artist_changes);
  }

  ui_lock();
  s_np_title = normalized_title;
  s_np_artist = normalized_artist;
  // 切歌时重置滚动偏移
  s_title_scroll_x = 0;
  s_artist_scroll_x = 0;
  s_scroll_last_ms = 0;
  ui_unlock();
  ui_request_refresh();
}

void ui_set_album(const String& album)
{
  String normalized_album = album;
  (void)text_normalize_display_spaces_inplace(normalized_album);
  normalized_album.trim();

  ui_lock();
  s_np_album = normalized_album;
  ui_unlock();
  // 切歌时重置专辑滚动偏移
  reset_album_scroll();
  ui_request_refresh();
}
