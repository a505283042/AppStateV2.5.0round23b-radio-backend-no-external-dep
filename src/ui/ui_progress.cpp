#include "ui/ui_progress.h"
#include "ui/ui_text_utils.h"
#include "ui/ui_icons.h"
#include "ui/ui_colors.h"
#include "ui/ui.h"
#include "ui/ui_icon_images.h"
#include "audio/audio_service.h"

// 外部声明全局变量
extern lgfx::U8g2font g_font_cjk;
extern volatile uint8_t s_ui_volume;   // 0~100
extern volatile play_mode_t s_ui_play_mode;  // 播放模式
extern volatile int     s_ui_track_idx;  // 0-based
extern volatile int     s_ui_track_total;
extern String s_np_album;

// 专辑名滚动状态（像素偏移）
static int s_album_scroll_x = 0;
static uint32_t s_album_scroll_last_ms = 0;
static constexpr int ALBUM_SCROLL_SPEED = 1;    // 滚动速度（像素/帧）
static constexpr int ALBUM_SCROLL_GAP = 20;     // 副本间距（像素）

// 专辑名宽度缓存
static String s_last_album_name = "";
static int s_last_album_width = 0;

// 播放模式切换高亮状态
extern volatile uint32_t s_ui_mode_switch_time;
#define MODE_SWITCH_HIGHLIGHT_MS 2000  // 模式切换高亮时间（毫秒）

void draw_time_bar(LGFX_Sprite* dst,
                  int y_bar, int y_time,
                  uint32_t el_ms,
                  uint32_t total_ms,        // 0=未知
                  int safe_pad,
                  uint16_t c_text)
{
  const uint16_t c_bar_bg   = UI_COLOR_BAR_BG;     // 未播（森林绿）
  const uint16_t c_bar_play = UI_COLOR_BAR_PLAY;   // 已播（暖阳橙黄）

  int x0, w;
  circle_span(y_bar, safe_pad, x0, w);
  if (w < 60) return;

  const int margin = 5;
  const int bar_x  = x0 + margin;
  const int bar_w  = w - margin * 2;
  const int bar_h  = 6;

  // 滑动点：有总时长就按比例走；未知则每分钟循环
  int dot = 0;
  
  if (total_ms >= 1000 && total_ms != 0xFFFFFFFFu) {
    uint32_t clamped = el_ms > total_ms ? total_ms : el_ms;
    dot = (uint64_t)clamped * (bar_w - 2) / total_ms;
  } else {
    dot = (int)((el_ms / 1000) % 60) * (bar_w - 2) / 60;
  }
  if (dot < 0) dot = 0;
  if (dot > bar_w - 2) dot = bar_w - 2;

  // 画进度条
  dst->fillRect(bar_x, y_bar, bar_w, bar_h, c_bar_bg);
  int played_w = dot + 1;
  if (played_w > 0) {
    dst->fillRect(bar_x + 1, y_bar + 1, played_w, bar_h - 2, c_bar_play);
  }

  // 画光标头（2像素宽）
  const uint16_t c_cursor = UI_COLOR_BAR_CURSOR;  // 活力紫红
  int cursor_x = bar_x + 1 + dot;

  // ===== 修改开始：增加闪烁逻辑 =====
  bool show_cursor = true;
  if (audio_service_is_paused()) {
    // 每 500ms 切换一次状态 (1秒一个周期)
    // 暂停时，光标会在显示 500ms 和 消失 500ms 之间循环
    if ((millis() / 500) % 2 == 0) {
      show_cursor = false;
    }
  }

  if (show_cursor) {
    dst->fillRect(cursor_x, y_bar, 2, bar_h, c_cursor);
  }
  // ===== 修改结束 =====

  // 时间文本
  char el[6];
  fmt_mmss(el_ms, el);

  char total_str[6];
  if (total_ms >= 1000 && total_ms != 0xFFFFFFFFu) fmt_mmss(total_ms, total_str);
  else { memcpy(total_str, "--:--", 6); }

  dst->setTextSize(1);

  // 左侧 elapsed
  {
    int xL0, wL;
    circle_span(y_time, safe_pad, xL0, wL);
    int x = xL0 + 5;
    String s(el);

    // 无描边
    // 如果暂停且处于闪烁的"隐藏"周期，就把文字颜色设为和背景一样（或者变暗）
    uint16_t current_text_color = c_text;
    if (audio_service_is_paused() && ((millis() / 500) % 2 == 0)) {
        current_text_color = c_bar_bg; // 变暗，或者直接用背景色隐藏
    }

    dst->setTextColor(current_text_color);
    dst->setCursor(x, y_time);
    dst->print(s);
  }

  // 右侧 total
  {
    circle_span(y_time, safe_pad, x0, w);
    int tw = dst->textWidth(total_str);
    int xr = x0 + w - tw - 5;

    // 无描边
    dst->setTextColor(c_text);
    dst->setCursor(xr, y_time);
    dst->print(total_str);
  }

  // ===== 时间行中间：曲目位置 idx/total =====
  char mid[16];
  if (s_ui_track_total > 0) snprintf(mid, sizeof(mid), "%d/%d", s_ui_track_idx + 1, s_ui_track_total);
  else                      snprintf(mid, sizeof(mid), "--/--");

  {
    int x0t, wt;
    circle_span(y_time, safe_pad, x0t, wt);

    int tw = dst->textWidth(mid);
    int xm = x0t + (wt - tw) / 2;

    // 简单防挤压：如果中间文本太宽就不画（一般不会）
    if (tw < wt - 80) {
      // 无描边
      dst->setTextColor(c_text);
      dst->setCursor(xm, y_time);
      dst->print(mid);
    }
  }
}

void draw_status_row(LGFX_Sprite* dst,
                    int y,
                    int safe_pad,
                    uint16_t fg,
                    bool volume_active)
{
  dst->setFont(&g_font_cjk);
  dst->setTextSize(1);

  int x0, w;
  circle_span(y, safe_pad, x0, w);
  if (w < 60) return;

  const int margin = 10;
  const int icon_size = 10;

  // 左侧：音量图标 + 百分比
  int xL = x0 + margin;
  uint16_t volume_color = volume_active ? UI_COLOR_VOLUME_ACTIVE : fg;
  draw_volume_icon(dst, xL, y + 3, volume_color);  // 上移2像素
  
  char vol_str[8];
  snprintf(vol_str, sizeof(vol_str), "%u%%", (unsigned)s_ui_volume);
  dst->setTextColor(volume_color);
  dst->setCursor(xL + 11 + 4, y);  // 上移2像素
  dst->print(vol_str);
  
  // 计算左侧音量区域的宽度
  int vol_width = 11 + 4 + dst->textWidth(vol_str);  // 图标 + 间距 + 文本

  // 右侧：播放模式图标（根据播放模式显示不同的图标组合）
  int xR = x0 + w - icon_size * 2 - margin * 2 + 7;  // 右边图标位置不变
  
  // 计算右侧模式图标区域的宽度
  int mode_width = icon_size * 2 + margin * 2 - 7;  // 两个图标 + 边距 - 右移7像素

  // 检查是否处于模式切换高亮状态（2秒内）
  uint32_t now = millis();
  bool mode_highlight = (now - s_ui_mode_switch_time < MODE_SWITCH_HIGHLIGHT_MS);
  uint16_t mode_color = mode_highlight ? UI_COLOR_VOLUME_ACTIVE : fg;

  // 根据播放模式显示对应的图标
  // 左边图标往右移3像素，右边图标位置不变
  int xL_icon = xR + 3;  // 左边图标位置
  int xR_icon = xR + icon_size + 4;  // 右边图标位置不变
  
  switch (s_ui_play_mode) {
    case PLAY_MODE_ALL_SEQ:
      // 全部顺序：TF卡图标 + 顺序图标
      draw_tfcard_icon(dst, xL_icon, y - icon_size / 2 + 8, mode_color);
      draw_repeat_icon(dst, xR_icon, y - icon_size / 2 + 8, mode_color);
      break;

    case PLAY_MODE_ALL_RND:
      // 全部随机：TF卡图标 + 随机图标
      draw_tfcard_icon(dst, xL_icon, y - icon_size / 2 + 8, mode_color);
      draw_random_icon(dst, xR_icon, y - icon_size / 2 + 8, mode_color);
      break;

    case PLAY_MODE_ARTIST_SEQ:
      // 歌手顺序：歌手图标 + 顺序图标
      draw_artist_icon(dst, xL_icon, y - icon_size / 2 + 8, mode_color);
      draw_repeat_icon(dst, xR_icon, y - icon_size / 2 + 8, mode_color);
      break;

    case PLAY_MODE_ARTIST_RND:
      // 歌手随机：歌手图标 + 随机图标
      draw_artist_icon(dst, xL_icon, y - icon_size / 2 + 8, mode_color);
      draw_random_icon(dst, xR_icon, y - icon_size / 2 + 8, mode_color);
      break;

    case PLAY_MODE_ALBUM_SEQ:
      // 专辑顺序：专辑图标 + 顺序图标
      draw_album_icon(dst, xL_icon, y - icon_size / 2 + 8, mode_color);
      draw_repeat_icon(dst, xR_icon, y - icon_size / 2 + 8, mode_color);
      break;

    case PLAY_MODE_ALBUM_RND:
      // 专辑随机：专辑图标 + 随机图标
      draw_album_icon(dst, xL_icon, y - icon_size / 2 + 8, mode_color);
      draw_random_icon(dst, xR_icon, y - icon_size / 2 + 8, mode_color);
      break;
  }

  // 中间：专辑名（带专辑图标，空就给个占位）
  // 原来的绘制方法（已注释掉）：
  // String midS = s_np_album;
  // if (midS.length() == 0) midS = "未知专辑";
  // int available_width = w - vol_width - mode_width - margin;
  // int twM = dst->textWidth(midS.c_str());
  // if (twM > available_width) {
  //   int ellipsis_width = dst->textWidth("...");
  //   while ((twM + ellipsis_width) > available_width && midS.length() > 0) {
  //     midS = midS.substring(0, midS.length() - 1);
  //     twM = dst->textWidth(midS.c_str());
  //   }
  //   midS += "...";
  //   twM += ellipsis_width;
  // }
  // int xM = xL + vol_width + (available_width - twM) / 2;
  // dst->setTextColor(UI_COLOR_ALBUM);
  // dst->setCursor(xM, y);
  // dst->print(midS.c_str());

  // 新的绘制方法（带专辑图标）：
  String midS = s_np_album;
  if (midS.length() == 0) midS = "未知专辑";

  // 计算中间可用空间（音量区域结束位置到模式图标开始位置）
  // 左边模式图标往右移3像素，专辑名可用宽度减少3像素
  int available_width = w - vol_width - mode_width - margin - 3;

  const int ALBUM_ICON_W = 14;
  const int ALBUM_ICON_GAP = 2;

  // 计算专辑名文本最大可用宽度（减去图标和间距）
  int max_text_width = available_width - ALBUM_ICON_W - ALBUM_ICON_GAP;

  // 计算专辑名实际宽度（使用缓存优化）
  dst->setTextWrap(false);  // 禁止自动换行
  int twM = 0;
  
  // 检查专辑名是否变化，只有变化时才重新计算宽度
  if (midS != s_last_album_name) {
    twM = dst->textWidth(midS.c_str());
    // 更新缓存
    s_last_album_name = midS;
    s_last_album_width = twM;
  } else {
    // 使用缓存的宽度
    twM = s_last_album_width;
  }
  
  bool need_scroll = (twM > max_text_width);

  // 居中显示位置（图标+文字整体居中）
  int start_x = xL + vol_width;

  // 文本起始位置（图标右侧）
  int text_start_x = start_x + ALBUM_ICON_W + ALBUM_ICON_GAP;

  if (!need_scroll) {
    // 不需要滚动：图标+文本整体居中
    int total_width = ALBUM_ICON_W + ALBUM_ICON_GAP + twM;
    int offset_x = (available_width - total_width) / 2;
    
    // 绘制专辑图标（居中位置）
    int icon_y = y - ALBUM_ICON_W + 15;
    draw_album_icon_img(dst, start_x + offset_x, icon_y, UI_COLOR_ALBUM);
    
    // 绘制文本（居中位置）
    dst->setTextColor(UI_COLOR_ALBUM);
    dst->setCursor(text_start_x + offset_x, y);
    dst->print(midS.c_str());
  } else {
    // 需要滚动：图标固定在左边，文本滚动
    int icon_y = y - ALBUM_ICON_W + 15;
    draw_album_icon_img(dst, start_x, icon_y, UI_COLOR_ALBUM);
    
    // 更新滚动偏移（像素）
    uint32_t now = millis();
    if (now - s_album_scroll_last_ms > 30) {  // 每30ms滚动1像素
      s_album_scroll_last_ms = now;
      s_album_scroll_x += ALBUM_SCROLL_SPEED;
      // 滚动范围：文本宽度 + 间距
      if (s_album_scroll_x > twM + ALBUM_SCROLL_GAP) {
        s_album_scroll_x = 0;
      }
    }

    dst->setTextColor(UI_COLOR_ALBUM);

    // 主文本位置（像素滚动）
    int x1 = text_start_x - s_album_scroll_x;

    // 设置裁剪区域（限制在当前行高度内，防止遮挡上下元素）
    const int line_height = 18;  // 14像素字体的合适行高
    dst->setClipRect(text_start_x, y - 4, max_text_width, line_height);

    // 绘制主文本
    dst->setCursor(x1, y);
    dst->print(midS.c_str());

    // 绘制副本文本
    int x2 = x1 + twM + ALBUM_SCROLL_GAP;
    dst->setCursor(x2, y);
    dst->print(midS.c_str());

    // 清除裁剪区域
    dst->clearClipRect();
  }
}

void reset_album_scroll() {
  s_album_scroll_x = 0;
  s_album_scroll_last_ms = 0;
  // 重置专辑名缓存，确保下次重新计算宽度
  s_last_album_name = "";
  s_last_album_width = 0;
}