#pragma once

#include <stdint.h>

#include "ui/gc9a01_lgfx.h"

void draw_volume_icon(LGFX_Sprite* dst, int x, int y, uint16_t color);
void draw_random_icon(LGFX_Sprite* dst, int x, int y, uint16_t color);
void draw_repeat_icon(LGFX_Sprite* dst, int x, int y, uint16_t color);
void draw_artist_icon(LGFX_Sprite* dst, int x, int y, uint16_t color);
void draw_tfcard_icon(LGFX_Sprite* dst, int x, int y, uint16_t color);
void draw_album_icon(LGFX_Sprite* dst, int x, int y, uint16_t color);

// 规范化的单色点阵图标（按行位图，1=点亮）
void draw_alarm_icon(LGFX_Sprite* dst, int x, int y, uint16_t color);   // 10x10
void draw_sleep_icon(LGFX_Sprite* dst, int x, int y, uint16_t color);   // 11x10（原始数据含第11位）
void draw_nas_icon(LGFX_Sprite* dst, int x, int y, uint16_t color);     // 11x10
void draw_radio_icon(LGFX_Sprite* dst, int x, int y, uint16_t color);   // 10x10

// 图片图标函数 (14x14 RGB565)
void draw_icon_image(LGFX_Sprite* dst, int x, int y, const uint16_t* icon_data);
void draw_album_icon_img(LGFX_Sprite* dst, int x, int y, uint16_t color);
void draw_artist_icon_img(LGFX_Sprite* dst, int x, int y, uint16_t color);
void draw_note_icon_img(LGFX_Sprite* dst, int x, int y, uint16_t color);