#include "ui/ui_text_utils.h"
#include "ui/gc9a01_lgfx.h"
#include <math.h>

// 封面尺寸常量（需要与 ui.cpp 保持一致）
static constexpr int COVER_SIZE = 240;

// 滚动文本参数
static constexpr int SCROLL_GAP_CHARS = 2;  // 副本间距（字符数）

// 外部声明 TFT 对象
extern LGFX tft;

// UTF-8 字符长度计算
static int utf8_char_len(uint8_t c)
{
  if ((c & 0x80) == 0) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1;
}

// 获取指定位置的UTF-8字符宽度（像素）
static int get_char_width(LGFX_Sprite* dst, const String& text, int byte_pos)
{
  if (byte_pos >= (int)text.length()) return 0;
  
  int char_len = utf8_char_len((uint8_t)text[byte_pos]);
  // 使用临时缓冲区，避免创建 String 对象
  char temp_buf[5] = {0}; // UTF-8字符最多4字节
  const char* p = text.c_str() + byte_pos;
  memcpy(temp_buf, p, char_len);
  temp_buf[char_len] = '\0';
  return dst->textWidth(temp_buf);
}

// 计算从开头到指定字符位置的累计宽度
static int get_text_width_to_pos(lgfx::v1::LGFX_Device* dst, const String& text, int char_index)
{
  int width = 0;
  int byte_pos = 0;
  int current_char = 0;
  const char* p = text.c_str();
  char temp_buf[5] = {0}; // UTF-8字符最多4字节
  
  while (byte_pos < (int)text.length() && current_char < char_index) {
    int char_len = utf8_char_len((uint8_t)p[byte_pos]);
    // 使用临时缓冲区，避免创建 String 对象
    memcpy(temp_buf, p + byte_pos, char_len);
    temp_buf[char_len] = '\0';
    width += dst->textWidth(temp_buf);
    byte_pos += char_len;
    current_char++;
  }
  
  return width;
}

// 判断是否为可断字符
static bool is_break_char(uint8_t c)
{
  return c == ' ' || c == '-' || c == '/' || c == '&' || c == '(' || c == ')';
}

// UTF-8 文本两行换行
Wrapped2Line wrap_utf8_to_2lines(lgfx::v1::LGFX_Device* dst, const String& s, int max_w)
{
  Wrapped2Line out;
  String text = s;
  text.trim();
  if (text.isEmpty()) return out;

  // 一行能放下
  if (dst->textWidth(text) <= max_w) {
    out.line1 = text;
    return out;
  }

  // 找第一行最大可放字符位置（按 UTF-8 字符序号）
  int total_chars = 0;
  for (int i = 0; i < text.length();) {
    uint8_t c = (uint8_t)text[i];
    if ((c & 0x80) == 0) i += 1;
    else if ((c & 0xE0) == 0xC0) i += 2;
    else if ((c & 0xF0) == 0xE0) i += 3;
    else if ((c & 0xF8) == 0xF0) i += 4;
    else i += 1;
    total_chars++;
  }

  int best_fit = 0;
  for (int ci = 1; ci <= total_chars; ++ci) {
    int w = get_text_width_to_pos(dst, text, ci);
    if (w <= max_w) best_fit = ci;
    else break;
  }

  if (best_fit <= 0) {
    out.line1 = clip_utf8_by_px((LGFX_Sprite*)dst, text, max_w);
    out.truncated = true;
    return out;
  }

  // 优先往前找自然断点
  int split_pos = best_fit;
  int search_from = max(1, best_fit - 8);
  for (int ci = best_fit; ci >= search_from; --ci) {
    // 取到第 ci 个字符之前的文本
    int byte_end = 0;
    int count = 0;
    for (int i = 0; i < text.length();) {
      if (count == ci) break;
      uint8_t c = (uint8_t)text[i];
      int step = 1;
      if ((c & 0x80) == 0) step = 1;
      else if ((c & 0xE0) == 0xC0) step = 2;
      else if ((c & 0xF0) == 0xE0) step = 3;
      else if ((c & 0xF8) == 0xF0) step = 4;
      i += step;
      byte_end = i;
      count++;
    }

    if (byte_end > 0 && byte_end <= text.length()) {
      uint8_t prev = (uint8_t)text[byte_end - 1];
      if (is_break_char(prev)) {
        split_pos = ci;
        break;
      }
    }
  }

  // 按字符位置转 byte 位置
  auto char_pos_to_byte = [&](const String& str, int char_pos) -> int {
    int count = 0;
    for (int i = 0; i < str.length();) {
      if (count == char_pos) return i;
      uint8_t c = (uint8_t)str[i];
      int step = 1;
      if ((c & 0x80) == 0) step = 1;
      else if ((c & 0xE0) == 0xC0) step = 2;
      else if ((c & 0xF0) == 0xE0) step = 3;
      else if ((c & 0xF8) == 0xF0) step = 4;
      i += step;
      count++;
    }
    return str.length();
  };

  int split_byte = char_pos_to_byte(text, split_pos);
  out.line1 = text.substring(0, split_byte);
  out.line1.trim();

  String remain = text.substring(split_byte);
  remain.trim();

  if (remain.isEmpty()) {
    return out;
  }

  if (dst->textWidth(remain) <= max_w) {
    out.line2 = remain;
    return out;
  }

  out.line2 = clip_utf8_by_px((LGFX_Sprite*)dst, remain, max_w);
  out.truncated = true;
  return out;
}

// 绘制居中的两行文本
void draw_centered_wrapped_2lines(lgfx::v1::LGFX_Device* dst,
                                  const String& text,
                                  int cx,
                                  int y1,
                                  int line_h,
                                  int max_w,
                                  uint16_t color)
{
  Wrapped2Line w = wrap_utf8_to_2lines(dst, text, max_w);

  dst->setTextDatum(middle_center);
  dst->setTextColor(color);
  dst->drawString(w.line1, cx, y1);

  if (!w.line2.isEmpty()) {
    dst->drawString(w.line2, cx, y1 + line_h);
  }
}

void draw_center_text(const char* s, int y)
{
  extern lgfx::U8g2font g_font_cjk;
  tft.setFont(&g_font_cjk);
  int16_t x = (COVER_SIZE - tft.textWidth(s)) / 2;
  if (x < 0) x = 0;
  tft.setCursor(x, y);
  tft.print(s);
}

// 查找表缓存：预计算所有 y 值对应的 (x0, w) 结果
// 屏幕尺寸固定为 240x240，y 范围 0-239
struct CircleSpanLutEntry {
  uint16_t x0;
  uint16_t w;
};

static bool s_circle_lut_init = false;
static CircleSpanLutEntry s_circle_lut[240][10]; // [y][pad]

void init_circle_lut()
{
  if (s_circle_lut_init) return;

  const int cx = COVER_SIZE / 2;   // 120
  const int cy = COVER_SIZE / 2;   // 120
  const int r  = COVER_SIZE / 2;   // 120

  for (int y = 0; y < 240; y++) {
    int dy = y - cy;
    int ady = dy < 0 ? -dy : dy;

    if (ady >= r) {
      for (int pad = 0; pad < 10; pad++) {
        s_circle_lut[y][pad].x0 = (uint16_t)cx;
        s_circle_lut[y][pad].w = 0;
      }
      continue;
    }

    float dx = sqrtf((float)(r * r - ady * ady));

    for (int pad = 0; pad < 10; pad++) {
      int xmin = (int)ceilf(cx - dx) + pad;
      int xmax = (int)floorf(cx + dx) - pad;

      if (xmax < xmin) {
        s_circle_lut[y][pad].x0 = (uint16_t)cx;
        s_circle_lut[y][pad].w = 0;
      } else {
        s_circle_lut[y][pad].x0 = (uint16_t)xmin;
        s_circle_lut[y][pad].w = (uint16_t)(xmax - xmin + 1);
      }
    }
  }

  s_circle_lut_init = true;
}

void circle_span(int y, int pad, int& x0, int& w)
{
  init_circle_lut();

  if (y < 0) y = 0;
  if (y >= 240) y = 239;
  if (pad < 0) pad = 0;
  if (pad >= 10) pad = 9;

  x0 = (int)s_circle_lut[y][pad].x0;
  w  = (int)s_circle_lut[y][pad].w;
}

void fmt_mmss(uint32_t ms, char out[6])
{
  uint32_t s = ms / 1000;
  uint32_t m = s / 60;
  s %= 60;
  if (m > 99) { m = 99; s = 59; } // 封顶处理，显示 99:59
  snprintf(out, 6, "%02u:%02u", (unsigned)m, (unsigned)s);
}

// 按像素宽度截断文本（末尾加 ...）
// 重构：直接调用 clip_utf8_by_px 以减少代码重复
String clip_text_px(LGFX_Sprite* dst, const String& s, int max_w)
{
  return clip_utf8_by_px(dst, s, max_w);
}

// UTF-8 像素级文本截断：按最大像素宽度截断，正确处理UTF-8字符（末尾加 ...）
String clip_utf8_by_px(LGFX_Sprite* dst, const String& s, int max_w)
{
  if (!dst) return s;
  
  // 先检查完整文本宽度
  int full_w = dst->textWidth(s.c_str());
  if (full_w <= max_w) return s;

  // 计算 ... 的宽度
  int dots_w = dst->textWidth("...");
  int avail = max_w - dots_w;
  if (avail < 10) avail = 10;  // 最小可用宽度

  String out;
  int out_w = 0;
  const char* p = s.c_str();
  
  while (*p) {
    // 获取UTF-8字符长度
    int clen = utf8_char_len((uint8_t)*p);
    if (clen < 1) clen = 1;
    
    // 提取单个字符
    String ch;
    for (int i = 0; i < clen && *p; i++) {
      ch += *p++;
    }
    
    // 计算字符宽度
    int cw = dst->textWidth(ch.c_str());
    
    // 如果加上这个字符会超出可用宽度，停止
    if (out_w + cw > avail) {
      break;
    }
    
    out += ch;
    out_w += cw;
  }
  
  out += "...";
  return out;
}


void draw_title_with_note(LGFX_Sprite* dst,
                          int y,
                          const String& text,
                          int text_size,
                          uint16_t fg,
                          int safe_pad)
{
  extern lgfx::U8g2font g_font_cjk;
  extern void draw_note_icon_img(LGFX_Sprite* dst, int x, int y, uint16_t color);

  dst->setFont(&g_font_cjk);
  dst->setTextSize(text_size);

  int x0, w;
  circle_span(y, safe_pad, x0, w);
  if (w <= 30) return; // 需要足够空间显示图标+文字

  const int ICON_W = 14;
  const int ICON_H = 14;
  const int ICON_GAP = 2;

  int text_max_w = w - ICON_W - ICON_GAP;
  if (text_max_w < 20) return;

  String t = clip_utf8_by_px(dst, text, text_max_w);
  if (t.length() == 0) return;

  int tw = dst->textWidth(t.c_str());
  int total_w = ICON_W + ICON_GAP + tw;

  int start_x = x0 + (w - total_w) / 2;

  int icon_y = y - ICON_H + 16;
  draw_note_icon_img(dst, start_x, icon_y, fg);

  int text_x = start_x + ICON_W + ICON_GAP;
  dst->setTextColor(fg);
  dst->setCursor(text_x, y);
  dst->print(t);
}

void draw_artist_with_icon(LGFX_Sprite* dst,
                           int y,
                           const String& text,
                           int text_size,
                           uint16_t fg,
                           int safe_pad)
{
  extern lgfx::U8g2font g_font_cjk;
  extern void draw_artist_icon_img(LGFX_Sprite* dst, int x, int y, uint16_t color);

  dst->setFont(&g_font_cjk);
  dst->setTextSize(text_size);

  int x0, w;
  circle_span(y, safe_pad, x0, w);
  if (w <= 30) return;

  const int ICON_W = 14;
  const int ICON_H = 14;
  const int ICON_GAP = 2;

  int text_max_w = w - ICON_W - ICON_GAP;
  if (text_max_w < 20) return;

  String t = clip_utf8_by_px(dst, text, text_max_w);
  if (t.length() == 0) return;

  int tw = dst->textWidth(t.c_str());
  int total_w = ICON_W + ICON_GAP + tw;

  int start_x = x0 + (w - total_w) / 2;

  int icon_y = y - ICON_H + 16;
  draw_artist_icon_img(dst, start_x, icon_y, fg);

  int text_x = start_x + ICON_W + ICON_GAP;
  dst->setTextColor(fg);
  dst->setCursor(text_x, y);
  dst->print(t);
}

void draw_album_with_icon(LGFX_Sprite* dst,
                          int y,
                          const String& text,
                          int text_size,
                          uint16_t fg,
                          int safe_pad)
{
  extern lgfx::U8g2font g_font_cjk;
  extern void draw_album_icon_img(LGFX_Sprite* dst, int x, int y, uint16_t color);

  dst->setFont(&g_font_cjk);
  dst->setTextSize(text_size);

  int x0, w;
  circle_span(y, safe_pad, x0, w);
  if (w <= 30) return;

  const int ICON_W = 14;
  const int ICON_H = 14;
  const int ICON_GAP = 2;

  int text_max_w = w - ICON_W - ICON_GAP;
  if (text_max_w < 20) return;

  String t = clip_utf8_by_px(dst, text, text_max_w);
  if (t.length() == 0) return;

  int tw = dst->textWidth(t.c_str());
  int total_w = ICON_W + ICON_GAP + tw;

  int start_x = x0 + (w - total_w) / 2;

  int icon_y = y - ICON_H + 16;
  draw_album_icon_img(dst, start_x, icon_y, fg);

  int text_x = start_x + ICON_W + ICON_GAP;
  dst->setTextColor(fg);
  dst->setCursor(text_x, y);
  dst->print(t);
}

// 在精灵上绘制居中文本（带颜色参数）
void draw_center_text_on_sprite(LGFX_Sprite* dst,
                                const char* s,
                                int y,
                                uint16_t fg,
                                int safe_pad)
{
  extern lgfx::U8g2font g_font_cjk;
  
  if (!dst || !s || s[0] == '\0') return;
  
  dst->setFont(&g_font_cjk);
  dst->setTextSize(1);
  
  // 计算圆屏安全区域
  int x0, w;
  circle_span(y, safe_pad, x0, w);
  if (w <= 10) return;
  
  // 截断文本以适应宽度
  String t = clip_utf8_by_px(dst, String(s), w);
  if (t.length() == 0) return;
  
  // 计算居中位置
  int tw = dst->textWidth(t.c_str());
  int x = x0 + (w - tw) / 2;
  if (x < x0) x = x0;
  
  // 绘制文本
  dst->setTextColor(fg);
  dst->setCursor(x, y);
  dst->print(t);
}

// 滚动文本绘制（支持副本滚动）
bool draw_scrolling_text(LGFX_Sprite* dst,
                         int y,
                         const String& text,
                         int& scroll_x,
                         int max_w,
                         uint16_t fg,
                         int gap)
{
  extern lgfx::U8g2font g_font_cjk;
  
  if (!dst || text.length() == 0) return false;
  
  dst->setFont(&g_font_cjk);
  dst->setTextSize(1);
  
  int text_w = dst->textWidth(text.c_str());
  
  // 如果文本不需要滚动
  if (text_w <= max_w) {
    int x = -scroll_x;  // 使用 scroll_x 作为偏移（通常为0）
    if (x < 0) x = 0;
    dst->setTextColor(fg);
    dst->setCursor(x, y);
    dst->print(text);
    return false;
  }
  
  // 需要滚动：绘制文本 + 副本
  dst->setTextColor(fg);
  
  // 设置裁剪区域
  int clip_x1 = 0;
  int clip_x2 = max_w;
  
  // 绘制原始文本
  int x1 = -scroll_x;
  if (x1 + text_w > clip_x1 && x1 < clip_x2) {
    dst->setCursor(x1, y);
    dst->print(text);
  }
  
  // 绘制副本文本
  int x2 = -scroll_x + text_w + gap;
  if (x2 + text_w > clip_x1 && x2 < clip_x2) {
    dst->setCursor(x2, y);
    dst->print(text);
  }
  
  return true;
}

// 绘制带图标的滚动文本（像素滚动）
bool draw_scrolling_text_with_icon(LGFX_Sprite* dst,
                                   int y,
                                   const String& text,
                                   int& scroll_x,
                                   int icon_w,
                                   uint16_t fg,
                                   int safe_pad,
                                   void (*draw_icon)(LGFX_Sprite*, int, int, uint16_t))
{
  extern lgfx::U8g2font g_font_cjk;
  
  if (!dst || text.length() == 0) return false;
  
  dst->setFont(&g_font_cjk);
  dst->setTextSize(1);
  dst->setTextWrap(false);
  
  // 计算圆屏安全区域
  int x0, w;
  circle_span(y, safe_pad, x0, w);
  if (w <= 30) return false;
  
  const int ICON_GAP = 2;
  const int SCROLL_GAP = 20;  // 副本间距（像素）
  int text_max_w = w - icon_w - ICON_GAP;
  if (text_max_w < 20) return false;
  
  int text_w = dst->textWidth(text.c_str());
  bool need_scroll = (text_w > text_max_w);
  
  if (!need_scroll) {
    // 不需要滚动：居中显示图标+文本
    int total_w = icon_w + ICON_GAP + text_w;
    int start_x = x0 + (w - total_w) / 2;
    
    if (draw_icon) {
      draw_icon(dst, start_x, y - 14 + 16, fg);
    }
    
    dst->setTextColor(fg);
    dst->setCursor(start_x + icon_w + ICON_GAP, y);
    dst->print(text);
    
    return false;
  }
  
  // 需要滚动
  int start_x = x0;
  
  // 绘制图标（固定位置）
  if (draw_icon) {
    draw_icon(dst, start_x, y - 14 + 16, fg);
  }
  
  int text_start_x = start_x + icon_w + ICON_GAP;
  int text_max_x = x0 + w;
  
  // 设置裁剪区域
  dst->setClipRect(text_start_x, 0, text_max_x - text_start_x, dst->height());
  dst->setTextColor(fg);
  
  // 主文本位置（像素滚动）
  int x1 = text_start_x - scroll_x;
  dst->setCursor(x1, y);
  dst->print(text);
  
  // 副本文本位置
  int x2 = x1 + text_w + SCROLL_GAP;
  dst->setCursor(x2, y);
  dst->print(text);
  
  dst->clearClipRect();
  
  return true;
}

// 绘制滚动文本（无图标）