#pragma once

#include <Arduino.h>
#include "ui/gc9a01_lgfx.h"

// 绘制时间进度条
// 参数: dst - 目标精灵, y_bar - 进度条Y坐标, y_time - 时间文本Y坐标
//       el_ms - 已播放毫秒, total_ms - 总毫秒, safe_pad - 安全边距
//       c_text - 文本颜色
void draw_time_bar(LGFX_Sprite* dst,
                  int y_bar, int y_time,
                  uint32_t el_ms,
                  uint32_t total_ms,        // 0=未知
                  int safe_pad,
                  uint16_t c_text);

// 绘制状态栏（音量、模式、列表信息）
// 参数: dst - 目标精灵, y - Y坐标, safe_pad - 安全边距
//       fg - 前景色, volume_active - 音量是否处于激活状态
void draw_status_row(LGFX_Sprite* dst,
                    int y,
                    int safe_pad,
                    uint16_t fg,
                    bool volume_active);

// 重置专辑名滚动偏移（切歌时调用）
void reset_album_scroll();