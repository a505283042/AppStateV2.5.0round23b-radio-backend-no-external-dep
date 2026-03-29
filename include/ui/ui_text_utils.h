#pragma once

#include <Arduino.h>
#include "ui/gc9a01_lgfx.h"

// 两行结果结构体
struct Wrapped2Line {
  String line1;
  String line2;
  bool truncated = false;
};

// 圆形边界计算：在圆屏的某个 y 位置，计算可用的 x 起点和宽度
// 参数: y - Y 坐标, pad - 左右边距, x0 - 输出起始 X, w - 输出宽度
void circle_span(int y, int pad, int& x0, int& w);

// 时间格式化：将毫秒转换为 MM:SS 格式
// 参数: ms - 毫秒数, out - 输出缓冲区（至少 6 字节）
void fmt_mmss(uint32_t ms, char out[6]);

// 像素级文本截断：按最大像素宽度截断（末尾加 ...）
// 参数: dst - 目标精灵, s - 原始字符串, max_w - 最大宽度
// 返回: 截断后的字符串
String clip_text_px(LGFX_Sprite* dst, const String& s, int max_w);

// UTF-8 像素级文本截断：按最大像素宽度截断，正确处理UTF-8字符（末尾加 ...）
// 参数: dst - 目标精灵, s - 原始字符串, max_w - 最大宽度
// 返回: 截断后的字符串
String clip_utf8_by_px(LGFX_Sprite* dst, const String& s, int max_w);

// UTF-8 文本两行换行
// 参数: dst - 目标设备, s - 原始字符串, max_w - 最大宽度
// 返回: 包含两行文本和截断状态的结构体
Wrapped2Line wrap_utf8_to_2lines(lgfx::v1::LGFX_Device* dst, const String& s, int max_w);

// 绘制居中的两行文本
// 参数: dst - 目标设备, text - 文本内容, cx - 中心X坐标, y1 - 第一行Y坐标
//       line_h - 行高, max_w - 最大宽度, color - 文本颜色
void draw_centered_wrapped_2lines(lgfx::v1::LGFX_Device* dst, 
                                  const String& text, 
                                  int cx, 
                                  int y1, 
                                  int line_h, 
                                  int max_w, 
                                  uint16_t color);

// 居中描边文本绘制（已注释掉，改用带图标的版本）
// 参数: dst - 目标精灵, y - 文本基线 y, text - 文本内容, text_size - 文本大小
//       fg - 前景色, safe_pad - 安全边距
// void draw_text_center_outline(LGFX_Sprite* dst,
//                            int y,
//                            const String& text,
//                            int text_size,
//                            uint16_t fg,
//                            int safe_pad);

// 居中文本绘制（直接在屏幕上绘制）
// 参数: s - 要绘制的文本字符串, y - 文本的 Y 坐标位置
void draw_center_text(const char* s, int y);

// 绘制带音符图标的居中文字（歌曲名）
// 音符图标放在文字前面，整体居中计算
// 参数: dst - 目标精灵, y - 文本基线 y, text - 文本内容, text_size - 文本大小
//       fg - 前景色, safe_pad - 安全边距
void draw_title_with_note(LGFX_Sprite* dst,
                          int y,
                          const String& text,
                          int text_size,
                          uint16_t fg,
                          int safe_pad);

// 绘制带歌手图标的居中文字（歌手名）
void draw_artist_with_icon(LGFX_Sprite* dst,
                           int y,
                           const String& text,
                           int text_size,
                           uint16_t fg,
                           int safe_pad);

// 绘制带专辑图标的居中文字（专辑名）
void draw_album_with_icon(LGFX_Sprite* dst,
                          int y,
                          const String& text,
                          int text_size,
                          uint16_t fg,
                          int safe_pad);

// 在精灵上绘制居中文本（带颜色参数）
// 参数: dst - 目标精灵, s - 文本内容, y - Y坐标, fg - 前景色, safe_pad - 安全边距
void draw_center_text_on_sprite(LGFX_Sprite* dst,
                                const char* s,
                                int y,
                                uint16_t fg,
                                int safe_pad);

// 滚动文本绘制（支持副本滚动）
// 参数: dst - 目标精灵, y - Y坐标, text - 文本内容, scroll_x - 滚动偏移（会被更新）
//       max_w - 最大显示宽度, fg - 前景色, gap - 副本间距
// 返回: 文本是否超出显示区域（需要滚动）
bool draw_scrolling_text(LGFX_Sprite* dst,
                         int y,
                         const String& text,
                         int& scroll_x,
                         int max_w,
                         uint16_t fg,
                         int gap = 40);

// 绘制带图标的滚动文本
// 参数: dst - 目标精灵, y - Y坐标, text - 文本内容, scroll_x - 滚动偏移
//       icon_w - 图标宽度, fg - 前景色, safe_pad - 安全边距, draw_icon - 图标绘制回调
// 返回: 文本是否超出显示区域
bool draw_scrolling_text_with_icon(LGFX_Sprite* dst,
                                   int y,
                                   const String& text,
                                   int& scroll_x,
                                   int icon_w,
                                   uint16_t fg,
                                   int safe_pad,
                                   void (*draw_icon)(LGFX_Sprite*, int, int, uint16_t));