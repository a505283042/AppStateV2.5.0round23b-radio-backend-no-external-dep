#pragma once

#include "ui/gc9a01_lgfx.h"

void draw_volume_icon(LGFX_Sprite* dst, int x, int y, uint16_t color);
void draw_random_icon(LGFX_Sprite* dst, int x, int y, uint16_t color);
void draw_repeat_icon(LGFX_Sprite* dst, int x, int y, uint16_t color);
void draw_artist_icon(LGFX_Sprite* dst, int x, int y, uint16_t color);
void draw_tfcard_icon(LGFX_Sprite* dst, int x, int y, uint16_t color);
void draw_album_icon(LGFX_Sprite* dst, int x, int y, uint16_t color);

// 图片图标函数 (14x14 RGB565)
void draw_icon_image(LGFX_Sprite* dst, int x, int y, const uint16_t* icon_data);
void draw_album_icon_img(LGFX_Sprite* dst, int x, int y, uint16_t color);
void draw_artist_icon_img(LGFX_Sprite* dst, int x, int y, uint16_t color);
void draw_note_icon_img(LGFX_Sprite* dst, int x, int y, uint16_t color);