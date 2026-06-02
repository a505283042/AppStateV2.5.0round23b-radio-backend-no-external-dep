#include <Arduino.h>
#include <WiFi.h>
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
#include "utils/log.h"
#include "web/web_settings.h"

#undef LOG_TAG
#define LOG_TAG "UI"

static void cover_set_source(LGFX_Sprite* src)
{
  s_src = src;
}

// 将旋转的封面渲染到后帧并推送到 LCD（稳定路径）
// 参数: angle_deg - 旋转角度（度）
// 功能: 将源精灵旋转指定角度后绘制到后帧，然后推送到屏幕
//      使用双缓冲技术避免闪烁，交换前后帧索引
void cover_rotate_draw(float angle_deg)
{
  // 检查旋转帧是否已初始化以及源精灵是否有效
  if (!s_rotFramesInited || !s_src) return;

  // 获取后帧指针（旋转专用双缓冲）
  auto* dst = s_rotFrame[s_rotBack];
  // 清空后帧
  dst->fillScreen(TFT_BLACK);
  // 将源精灵旋转指定角度并绘制到后帧（不缩放）
  s_src->pushRotateZoom(dst, COVER_SIZE / 2, COVER_SIZE / 2, angle_deg, 1.0f, 1.0f);

  // 将后帧推送到屏幕 (0, 0) 位置
  dst->pushSprite(0, 0);

  // 交换前后帧索引（双缓冲）
  uint8_t tmp = s_rotFront;
  s_rotFront = s_rotBack;
  s_rotBack = tmp;
}

static void draw_cover_panel_status_icons(LGFX_Sprite* dst, int center_y, uint16_t fg)
{
  if (!dst) return;

  dst->setFont(&g_font_cjk);
  dst->setTextSize(1);
  dst->setTextWrap(false);

  const int panel_x = 8;
  const int panel_w = 224;
  const int margin = 4;
  const int icon_size = 10;

  const bool volume_active =
      (millis() - s_ui_volume_active_time) < VOLUME_ACTIVE_TIMEOUT_MS;

  const uint16_t volume_color =
      volume_active ? UI_COLOR_VOLUME_ACTIVE : fg;

  // 左侧音量：贴近面板左边
  const int vol_icon_x = panel_x + margin;
  const int vol_icon_y = center_y - 5;

  draw_volume_icon(dst, vol_icon_x, vol_icon_y, volume_color);

  char vol_str[8];
  snprintf(vol_str, sizeof(vol_str), "%u%%", (unsigned)s_ui_volume);

  dst->setTextColor(volume_color);
  dst->setCursor(vol_icon_x + 15, center_y - 8);
  dst->print(vol_str);

  // 右侧模式：贴近面板右边
  const uint32_t now = millis();
  const bool mode_highlight =
      (now - s_ui_mode_switch_time) < MODE_SWITCH_HIGHLIGHT_MS;

  const uint16_t mode_color =
      mode_highlight ? UI_COLOR_VOLUME_ACTIVE : fg;

  const int right_icon_x = panel_x + panel_w - margin - icon_size;
  const int left_icon_x  = right_icon_x - icon_size - 4;
  const int icon_y       = center_y - 5;

  switch (s_ui_play_mode) {
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
static volatile int8_t s_cover_panel_nav_feedback = 0;
static volatile uint32_t s_cover_panel_nav_feedback_until_ms = 0;

// 通知封面面板导航反馈（上/下）
// 导航反馈会持续 320ms，超过时间后自动清除。
void ui_notify_cover_panel_nav_feedback(int8_t dir)
{
  if (dir < 0) {
    s_cover_panel_nav_feedback = -1;
  } else if (dir > 0) {
    s_cover_panel_nav_feedback = 1;
  } else {
    s_cover_panel_nav_feedback = 0;
  }

  // COVER_PANEL 帧率可能只有 8FPS，时间太短可能看不到
  s_cover_panel_nav_feedback_until_ms = millis() + 320;
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


static void draw_upper_rotated_cover_sampled(LGFX_Sprite* dst, LGFX_Sprite* src, float angle_deg)
{
  if (!dst || !src) return;

  dst->fillScreen(TFT_BLACK);

  uint16_t* sbuf = reinterpret_cast<uint16_t*>(src->getBuffer());
  uint16_t* dbuf = reinterpret_cast<uint16_t*>(dst->getBuffer());

  const float rad = angle_deg * 0.01745329252f;
  const int c = (int)(cosf(rad) * 1024.0f);
  const int s = (int)(sinf(rad) * 1024.0f);

  for (int y = 0; y < COVER_PANEL_Y; ++y) {
    int x0 = 0;
    int w = 0;
    circle_span(y, 0, x0, w);
    if (w <= 0) continue;

    const int yoff = y * COVER_SIZE;
    const int dy = y - COVER_SIZE / 2;

    for (int x = x0; x < x0 + w; ++x) {
      const int dx = x - COVER_SIZE / 2;

      // 反向映射：屏幕点 -> 原封面点
      const int sx = COVER_SIZE / 2 + ((dx * c + dy * s) >> 10);
      const int sy = COVER_SIZE / 2 + ((-dx * s + dy * c) >> 10);

      int src_x = sx;
      int src_y = sy;

      if (src_x < 0) src_x = 0;
      if (src_x >= COVER_SIZE) src_x = COVER_SIZE - 1;
      if (src_y < 0) src_y = 0;
      if (src_y >= COVER_SIZE) src_y = COVER_SIZE - 1;

      const uint16_t color = sbuf ? sbuf[src_y * COVER_SIZE + src_x] : src->readPixel(src_x, src_y);
      if (dbuf) dbuf[yoff + x] = color;
      else      dst->drawPixel(x, y, color);
    }
  }
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

  const int r = (br * inv + fr * alpha) / 255;
  const int g = (bgc * inv + fgc * alpha) / 255;
  const int b = (bb * inv + fb * alpha) / 255;

  return (uint16_t)((r << 11) | (g << 5) | b);
}

static void fill_blend_circle_rgb565(LGFX_Sprite* dst,
                                     int cx,
                                     int cy,
                                     int r,
                                     int skip_inner_r,
                                     uint16_t color,
                                     uint8_t alpha)
{
  if (!dst || r <= 0 || alpha == 0) return;

  const int skip_inner_r2 = skip_inner_r * skip_inner_r;

  for (int y = cy - r; y <= cy + r; ++y) {
    if ((unsigned)y >= 240) continue;

    const int dy = y - cy;
    const int xx = r * r - dy * dy;
    if (xx < 0) continue;

    const int half = (int)sqrtf((float)xx);

    int x0 = cx - half;
    int x1 = cx + half;

    if (x0 < 0) x0 = 0;
    if (x1 > 239) x1 = 239;

    for (int x = x0; x <= x1; ++x) {
      const int dx = x - cx;

      // 关键：最内圈不参与半透明混合
      if (skip_inner_r > 0 && (dx * dx + dy * dy) <= skip_inner_r2) {
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

  // 大唱片圆：半透明，但跳过最内圈
  fill_blend_circle_rgb565(dst,
                           cx,
                           cy,
                           r,
                           COVER_PANEL_RECORD_INNER_R,
                           TFT_BLACK,
                           COVER_PANEL_RECORD_ALPHA);

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

static bool cover_panel_inner_span(int y, int& x0, int& w)
{
  static constexpr int CX = 120;
  static constexpr int CY = 120;
  const int R = COVER_PANEL_INNER_R;

  const int dy = y - CY;
  if (dy < -R || dy > R) {
    x0 = 0;
    w = 0;
    return false;
  }

  const int half = (int)sqrtf((float)(R * R - dy * dy));
  x0 = CX - half;
  w  = half * 2 + 1;

  if (x0 < 0) {
    w += x0;
    x0 = 0;
  }

  if (x0 + w > 240) {
    w = 240 - x0;
  }

  return w > 0;
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

static bool cover_panel_skin_transparent_at_screen(int sx, int sy)
{
  const int lx = sx;
  const int ly = sy - COVER_PANEL_SKIN_Y;

  if (lx < 0 || lx >= COVER_PANEL_SKIN_W ||
      ly < 0 || ly >= COVER_PANEL_SKIN_H) {
    return true;
  }

  const uint16_t c =
      pgm_read_word(&g_cover_panel_skin_240x140[ly * COVER_PANEL_SKIN_W + lx]);

  return is_cover_panel_skin_transparent(c);
}

static void draw_pixel_if_skin_transparent(LGFX_Sprite* dst,
                                           int x,
                                           int y,
                                           uint16_t color)
{
  if (!dst) return;
  if ((unsigned)x >= 240 || (unsigned)y >= 240) return;

  if (!cover_panel_skin_transparent_at_screen(x, y)) {
    return;
  }

  dst->drawPixel(x, y, color);
}

static void draw_cover_panel_skin(LGFX_Sprite* dst)
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

static void draw_cover_panel_shell(LGFX_Sprite* dst)
{
  if (!dst) return;

  const uint16_t panel    = 0x1082;
  const uint16_t hub_line = 0x2104;

  // 下半圆面板：从 COVER_PANEL_Y 往下，只填充圆屏内部区域。
  for (int y = COVER_PANEL_Y; y < 240; ++y) {
    int x0 = 0;
    int w = 0;

    if (!cover_panel_inner_span(y, x0, w)) {
      continue;
    }

    dst->fillRect(x0, y, w, 1, panel);
  }

  // 上方小凸起，盖住封面。
  dst->fillCircle(120, COVER_PANEL_BUMP_CY, COVER_PANEL_BUMP_R, panel);

  // 补一下凸起和下方面板的连接处，避免中间有缝。
  dst->fillRect(120 - COVER_PANEL_BUMP_R,
                COVER_PANEL_Y,
                COVER_PANEL_BUMP_R * 2,
                COVER_PANEL_BUMP_R,
                panel);

  // 中间唱片孔 / 转轴。
  dst->fillCircle(120, COVER_PANEL_BUMP_CY, COVER_PANEL_HUB_R, TFT_BLACK);
  dst->drawCircle(120, COVER_PANEL_BUMP_CY, COVER_PANEL_HUB_R, hub_line);
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
  const bool feedback_alive =
      (int32_t)(s_cover_panel_nav_feedback_until_ms - now) > 0;

  const bool prev_active = feedback_alive && (s_cover_panel_nav_feedback < 0);
  const bool next_active = feedback_alive && (s_cover_panel_nav_feedback > 0);

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
  } else {
      draw_cover_panel_center_text(dst,
                                  "暂无歌词",
                                  COVER_PANEL_LYRIC_CUR_Y,
                                  c_lyrics_next,
                                  COVER_PANEL_TEXT_SAFE_PAD);
  }
}

static uint16_t lerp_rgb565(uint16_t c0, uint16_t c1, float t)
{
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;

  const int r0 = (c0 >> 11) & 0x1F;
  const int g0 = (c0 >> 5)  & 0x3F;
  const int b0 = c0 & 0x1F;

  const int r1 = (c1 >> 11) & 0x1F;
  const int g1 = (c1 >> 5)  & 0x3F;
  const int b1 = c1 & 0x1F;

  const int r = r0 + (int)((r1 - r0) * t);
  const int g = g0 + (int)((g1 - g0) * t);
  const int b = b0 + (int)((b1 - b0) * t);

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

      // 这里按“左 -> 右”算渐变比例：
      // deg = full_left_deg  时 t=0，蓝色
      // deg = full_right_deg 时 t=1，粉色
      float t = 0.0f;
      if (full_left_deg > full_right_deg) {
        t = (float)(full_left_deg - deg) / (float)full_range;
      } else {
        t = (float)(deg - full_left_deg) / (float)full_range;
      }

      const uint16_t color = lerp_rgb565(left_color, right_color, t);

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
  if (!s_rotFramesInited || !s_src) return;

  auto* dst = s_rotFrame[s_rotBack];

  // 1. 只画上半圆旋转封面
  draw_upper_rotated_cover_sampled(dst, s_src, angle_deg);

  // 2. 半透明唱片圆：封面前、面板后
  draw_cover_panel_record_overlay(dst);   
  
  // 3. 下半固定面板覆盖
  draw_cover_panel_skin(dst);

  // 4. 面板文字和按钮
  draw_cover_panel_info(dst);

  // 5. 最后画外圈进度弧，避免被面板覆盖
  draw_cover_panel_progress_ring(dst);

  dst->pushSprite(0, 0);

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

  // 4) WiFi信息显示（屏幕最上方）- 在遮罩之后绘制，确保可见
  const auto& ws = web_settings_get();
  if (ws.show_wifi_info) {
    const uint16_t c_wifi = 0xAD55;  // WiFi信息颜色（亮灰色）
    const int y_wifi_name = 19;      // WiFi名称显示位置
    const int y_wifi_ip = 34;        // IP地址显示位置
    
    // 获取WiFi名称和IP地址
    String wifiName = "-";
    String wifiIP = "0.0.0.0";
    
    if (WiFi.status() == WL_CONNECTED) {
      wifiName = WiFi.SSID();
      wifiIP = WiFi.localIP().toString();
    } else if (WiFi.getMode() == WIFI_AP) {
      wifiName = "AP模式";
      wifiIP = WiFi.softAPIP().toString();
    }
    
    // 分两行显示WiFi名称和IP地址
    draw_center_text_on_sprite(dst, wifiName.c_str(), y_wifi_name, c_wifi, safe_pad);
    draw_center_text_on_sprite(dst, wifiIP.c_str(), y_wifi_ip, c_wifi, safe_pad);
  }

  // 5) 歌词显示（屏幕上半部分）- 在遮罩之后绘制，确保可见
  bool hasLyrics = g_lyricsDisplay.hasLyrics();
  LOGD("[UI] hasLyrics: %d", hasLyrics);
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

  // 检查音量是否处于激活状态
  bool volume_active = (millis() - s_ui_volume_active_time) < VOLUME_ACTIVE_TIMEOUT_MS;
  draw_status_row(dst, y_status, safe_pad, c_time_text, volume_active);

  draw_time_bar(dst,
                y_bar, y_time,
                el_ms,
                total_ms,
                safe_pad,
                c_time_text);

  uint32_t t_status = millis();

  // 5) 标题/歌手（支持滚动显示长文本）
  extern void draw_note_icon_img(LGFX_Sprite* dst, int x, int y, uint16_t color);
  extern void draw_artist_icon_img(LGFX_Sprite* dst, int x, int y, uint16_t color);
  
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
      extern lgfx::U8g2font g_font_cjk;
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
      extern lgfx::U8g2font g_font_cjk;
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

  // 6) 推屏
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

  // 切歌：先清空总时长，等待播放层重新喂入
  s_ui_play_ms  = 0;
  s_ui_total_ms = 0;

  // 立即推送第一帧
  if (!s_screen_cleared) {
    tft.fillScreen(TFT_BLACK);
    s_screen_cleared = true;
  }
  s_coverSpr.pushSprite(0, 0);

  s_angle_deg = 0.0f;
  s_rot_last_ms = millis();
  ui_draw_unlock();
  ui_request_refresh();

  return true;
}

// =============================================================================
// 可选的覆盖层 API（目前保持为轻量级存根）
// =============================================================================
void ui_player_draw_overlay(const TrackInfo&, uint32_t, uint32_t,
                            const char*, const char*, const char*)
{
  // 最小化构建：无覆盖层
}

void ui_player_update_progress(uint32_t play_ms, uint32_t total_ms)
{
  s_ui_play_ms  = play_ms;
  s_ui_total_ms = total_ms;   // 0 表示未知

  g_lyricsDisplay.updateTime(play_ms);

  player_assets_try_apply_deferred_current_cover(player_state_current_index());

  ui_request_refresh();
}

void ui_player_update_lyrics(const char*, const char*)
{
  // 最小化构建：无覆盖层
}


enum ui_player_view_t ui_get_view() { return s_view; }

// 设置播放器视图（带线程锁保护）
// 参数: new_view - 新视图类型（ROTATE旋转封面 或 INFO信息详情）
// 功能: 更新视图状态，重置旋转时间戳防止角度跳变
void ui_set_view(ui_player_view_t new_view)
{
  const uint32_t now_ms = millis();
  ui_lock();
  s_view = new_view;
  s_rot_last_ms = now_ms;   // 防 dt 累积跳角度
  s_rotate_wait_audio_start = false;
  s_rotate_wait_prefetch_done = false;
  ui_unlock();
  ui_request_refresh();
}

// 切换播放器视图（长按PLAY键触发）
// 在旋转视图(ROTATE)和信息视图(INFO)之间切换
void ui_toggle_view()
{
  LOGI("[UI] toggle_view: current=%d", (int)s_view);

  ui_player_view_t next = UI_VIEW_INFO;

  switch (s_view) {
    case UI_VIEW_INFO:
      next = UI_VIEW_ROTATE;
      break;

    case UI_VIEW_ROTATE:
      next = UI_VIEW_COVER_PANEL;
      break;

    case UI_VIEW_COVER_PANEL:
      next = UI_VIEW_INFO;
      break;

    default:
      next = UI_VIEW_INFO;
      break;
  }

  ui_set_view(next);
  LOGI("[UI] toggle_view: new=%d", (int)s_view);
}

void ui_set_now_playing(const char* title, const char* artist)
{
  ui_lock();
  s_np_title  = title  ? String(title)  : String("");
  s_np_artist = artist ? String(artist) : String("");
  // 切歌时重置滚动偏移
  s_title_scroll_x = 0;
  s_artist_scroll_x = 0;
  s_scroll_last_ms = 0;
  ui_unlock();
  ui_request_refresh();
}

void ui_set_album(const String& album)
{
  ui_lock();
  s_np_album = album;
  ui_unlock();
  // 切歌时重置专辑滚动偏移
  reset_album_scroll();
  ui_request_refresh();
}

void ui_set_volume(uint8_t vol)
{
  if (vol > 100) vol = 100;
  s_ui_volume = vol;
  ui_request_refresh();
}

void ui_volume_key_pressed()
{
  s_ui_volume_active_time = millis();  // 更新音量激活时间
  ui_request_refresh();
}

void ui_set_play_mode(play_mode_t mode)
{
  s_ui_play_mode = mode;
  ui_request_refresh();
}

void ui_mode_switch_highlight()
{
  s_ui_mode_switch_time = millis();  // 记录模式切换时间，用于高亮显示
  ui_request_refresh();
}

void ui_set_track_pos(int idx, int total)
{
  if (total < 0) total = 0;
  if (idx < 0) idx = 0;
  s_ui_track_idx = idx;
  s_ui_track_total = total;
  ui_request_refresh();
}
