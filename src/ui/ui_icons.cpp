#include "ui/ui_icons.h"
#include "ui/gc9a01_lgfx.h"
#include "ui/ui_icon_images.h"

// 绘制音量图标
void draw_volume_icon(LGFX_Sprite* dst, int x, int y, uint16_t color)
{
  const int width = 11;  // 11像素宽
  const int height = 11; // 11像素高
  
  // 定义每列的线段（起始y坐标，长度），{start, len}，len=0表示结束
  const int segments[11][6][2] = {
    // 第1列：1段，y=4，长度4
    {{4, 4}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    // 第2列：1段，y=4，长度4
    {{4, 4}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    // 第3列：1段，y=3，长度6
    {{3, 6}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    // 第4列：1段，y=2，长度8
    {{2, 8}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    // 第5列：1段，y=1，长度10
    {{1, 10}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    // 第6列：空
    {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    // 第7列：2段，y=3长度1，y=8长度1
    {{3, 1}, {8, 1}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    // 第8列：4段
    {{1, 1}, {4, 1}, {7, 1}, {10, 1}, {0, 0}, {0, 0}},
    // 第9列：4段
    {{2, 1}, {5, 2}, {9, 1}, {0, 0}, {0, 0}, {0, 0}},
    // 第10列：2段
    {{3, 1}, {8, 1}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    // 第11列：1段，y=4，长度4
    {{4, 4}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}}
  };
  
  dst->setColor(color);
  
  // 遍历每一列，使用 fillRect 绘制垂直线段（宽度为1）
  for (int col = 0; col < width; col++) {
    for (int seg = 0; seg < 6; seg++) {
      int start = segments[col][seg][0];
      int len = segments[col][seg][1];
      if (len == 0) break; // 结束标记
      dst->fillRect(x + col, y + start, 1, len, color);
    }
  }
}

// 绘制随机播放图标
void draw_random_icon(LGFX_Sprite* dst, int x, int y, uint16_t color)
{
  const int icon_size = 10;

  // 定义每列的线段（起始y坐标，长度），{start, len}，len=0表示结束
  const int segments[10][6][2] = {
    // 第1列：2段，y=2长度1，y=7长度1
    {{2, 1}, {7, 1}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    // 第2列：2段，y=2长度1，y=7长度1
    {{2, 1}, {7, 1}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    // 第3列：2段，y=3长度1，y=6长度1
    {{3, 1}, {6, 1}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    // 第4列：1段，y=5长度1
    {{5, 1}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    // 第5列：1段，y=4长度1
    {{4, 1}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    // 第6列：2段，y=3长度1，y=6长度1
    {{3, 1}, {6, 1}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    // 第7列：2段，y=2长度1，y=7长度1
    {{2, 1}, {7, 1}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    // 第8列：6段
    {{0, 1}, {2, 1}, {4, 1}, {5, 1}, {7, 1}, {9, 1}},
    // 第9列：6段
    {{1, 1}, {2, 2}, {6, 1}, {7, 2}, {0, 0}, {0, 0}},
    // 第10列：2段，y=2长度1，y=7长度1
    {{2, 1}, {7, 1}, {0, 0}, {0, 0}, {0, 0}, {0, 0}}
  };

  dst->setColor(color);

  // 遍历每一列，使用 fillRect 绘制垂直线段（宽度为1）
  for (int col = 0; col < icon_size; col++) {
    for (int seg = 0; seg < 6; seg++) {
      int start = segments[col][seg][0];
      int len = segments[col][seg][1];
      if (len == 0) break; // 结束标记
      dst->fillRect(x + col, y + start, 1, len, color);
    }
  }
}

// 绘制专辑图标
void draw_album_icon(LGFX_Sprite* dst, int x, int y, uint16_t color)
{
  const int icon_size = 10;

  // 定义每列的线段（起始y坐标，长度），{start, len}，len=0表示结束
  const int segments[10][5][2] = {
    // 第1列：1段，y=3长度4
    {{3, 4}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    // 第2列：2段，y=2长度1，y=7长度1
    {{2, 1}, {7, 1}, {0, 0}, {0, 0}, {0, 0}},
    // 第3列：3段
    {{1, 1}, {5, 2}, {8, 1}, {0, 0}, {0, 0}},
    // 第4列：3段
    {{0, 1}, {7, 1}, {9, 1}, {0, 0}, {0, 0}},
    // 第5列：3段
    {{0, 1}, {4, 2}, {9, 1}, {0, 0}, {0, 0}},
    // 第6列：3段
    {{0, 1}, {4, 2}, {9, 1}, {0, 0}, {0, 0}},
    // 第7列：3段
    {{0, 1}, {2, 1}, {9, 1}, {0, 0}, {0, 0}},
    // 第8列：4段
    {{1, 1}, {3, 2}, {8, 1}, {0, 0}, {0, 0}},
    // 第9列：2段，y=2长度1，y=7长度1
    {{2, 1}, {7, 1}, {0, 0}, {0, 0}, {0, 0}},
    // 第10列：1段，y=3长度4
    {{3, 4}, {0, 0}, {0, 0}, {0, 0}, {0, 0}}
  };

  dst->setColor(color);

  // 遍历每一列，使用 fillRect 绘制垂直线段（宽度为1）
  for (int col = 0; col < icon_size; col++) {
    for (int seg = 0; seg < 5; seg++) {
      int start = segments[col][seg][0];
      int len = segments[col][seg][1];
      if (len == 0) break; // 结束标记
      dst->fillRect(x + col, y + start, 1, len, color);
    }
  }
}

// 绘制歌手图标
void draw_artist_icon(LGFX_Sprite* dst, int x, int y, uint16_t color)
{
  const int icon_size = 10;
  
  // 定义每列的线段（起始y坐标，长度），{start, len}，len=0表示结束
  const int segments[10][4][2] = {
    // 第1列：1段，y=8长度2
    {{8, 2}, {0, 0}, {0, 0}, {0, 0}},
    // 第2列：2段，y=7长度1，y=10长度1
    {{7, 1}, {10, 1}, {0, 0}, {0, 0}},
    // 第3列：3段
    {{3, 2}, {6, 1}, {10, 1}, {0, 0}},
    // 第4列：3段
    {{2, 1}, {5, 1}, {10, 1}, {0, 0}},
    // 第5列：3段
    {{1, 1}, {6, 1}, {10, 1}, {0, 0}},
    // 第6列：3段
    {{1, 1}, {6, 1}, {10, 1}, {0, 0}},
    // 第7列：3段
    {{2, 1}, {5, 1}, {10, 1}, {0, 0}},
    // 第8列：3段
    {{3, 2}, {6, 1}, {10, 1}, {0, 0}},
    // 第9列：2段，y=7长度1，y=10长度1
    {{7, 1}, {10, 1}, {0, 0}, {0, 0}},
    // 第10列：1段，y=8长度2
    {{8, 2}, {0, 0}, {0, 0}, {0, 0}}
  };
  
  dst->setColor(color);
  
  // 遍历每一列，使用 fillRect 绘制垂直线段（宽度为1）
  for (int col = 0; col < icon_size; col++) {
    for (int seg = 0; seg < 4; seg++) {
      int start = segments[col][seg][0];
      int len = segments[col][seg][1];
      if (len == 0) break; // 结束标记
      dst->fillRect(x + col, y + start, 1, len, color);
    }
  }
}

// 绘制文件夹图标
void draw_tfcard_icon(LGFX_Sprite* dst, int x, int y, uint16_t color)
{
  const int icon_size = 10;
  
  // 定义每列的线段（起始y坐标，长度），{start, len}，len=0表示结束
  const int segments[10][4][2] = {
    // 第1列：1段，y=2长度8
    {{2, 8}, {0, 0}, {0, 0}, {0, 0}},
    // 第2列：2段，y=2长度1，y=10长度1
    {{2, 1}, {10, 1}, {0, 0}, {0, 0}},
    // 第3列：3段
    {{1, 3}, {8, 3}, {0, 0}, {0, 0}},
    // 第4列：4段
    {{1, 1}, {3, 1}, {8, 1}, {10, 1}},
    // 第5列：3段
    {{1, 3}, {8, 1}, {10, 1}, {0, 0}},
    // 第6列：3段
    {{1, 1}, {8, 1}, {10, 1}, {0, 0}},
    // 第7列：3段
    {{2, 1}, {8, 3}, {0, 0}, {0, 0}},
    // 第8列：2段，y=3长度1，y=10长度1
    {{3, 1}, {10, 1}, {0, 0}, {0, 0}},
    // 第9列：1段，y=4长度6
    {{4, 6}, {0, 0}, {0, 0}, {0, 0}},
    // 第10列：空
    {{0, 0}, {0, 0}, {0, 0}, {0, 0}}
  };
  
  dst->setColor(color);
  
  // 遍历每一列，使用 fillRect 绘制垂直线段（宽度为1）
  for (int col = 0; col < icon_size; col++) {
    for (int seg = 0; seg < 4; seg++) {
      int start = segments[col][seg][0];
      int len = segments[col][seg][1];
      if (len == 0) break; // 结束标记
      dst->fillRect(x + col, y + start, 1, len, color);
    }
  }
}

// 绘制顺序播放图标
void draw_repeat_icon(LGFX_Sprite* dst, int x, int y, uint16_t color)
{
  const int icon_size = 10;
  
  // 定义每列的线段（起始y坐标，长度），{start, len}，len=0表示结束
  const int segments[10][4][2] = {
    // 第1列：2段，y=3长度2，y=7长度1
    {{3, 2}, {7, 1}, {0, 0}, {0, 0}},
    // 第2列：3段
    {{2, 1}, {6, 1}, {8, 1}, {0, 0}},
    // 第3列：4段
    {{2, 1}, {5, 1}, {7, 1}, {9, 1}},
    // 第4列：2段，y=2长度1，y=7长度1
    {{2, 1}, {7, 1}, {0, 0}, {0, 0}},
    // 第5列：2段，y=2长度1，y=7长度1
    {{2, 1}, {7, 1}, {0, 0}, {0, 0}},
    // 第6列：2段，y=2长度1，y=7长度1
    {{2, 1}, {7, 1}, {0, 0}, {0, 0}},
    // 第7列：2段，y=2长度1，y=7长度1
    {{2, 1}, {7, 1}, {0, 0}, {0, 0}},
    // 第8列：4段
    {{0, 1}, {2, 1}, {4, 1}, {7, 1}},
    // 第9列：3段
    {{1, 3}, {7, 1}, {0, 0}, {0, 0}},
    // 第10列：3段
    {{2, 1}, {5, 2}, {0, 0}, {0, 0}}
  };
  
  dst->setColor(color);
  
  // 遍历每一列，使用 fillRect 绘制垂直线段（宽度为1）
  for (int col = 0; col < icon_size; col++) {
    for (int seg = 0; seg < 4; seg++) {
      int start = segments[col][seg][0];
      int len = segments[col][seg][1];
      if (len == 0) break; // 结束标记
      dst->fillRect(x + col, y + start, 1, len, color);
    }
  }
}

// 绘制图片图标（14x14 RGB565，支持透明色）
void draw_icon_image(LGFX_Sprite* dst, int x, int y, const uint16_t* icon_data)
{
  // 使用 pushImage 的透明色版本
  // TFT_TRANSPARENT = 0x0120
  dst->pushImage(x, y, ICON_IMG_SIZE, ICON_IMG_SIZE, icon_data, TFT_TRANSPARENT);
}

// 绘制专辑图片图标
void draw_album_icon_img(LGFX_Sprite* dst, int x, int y, uint16_t color)
{
  (void)color; // 颜色参数未使用，图标自带颜色
  draw_icon_image(dst, x, y, ICON_ALBUM_IMG);
}

// 绘制歌手图片图标
void draw_artist_icon_img(LGFX_Sprite* dst, int x, int y, uint16_t color)
{
  (void)color;
  draw_icon_image(dst, x, y, ICON_ARTIST_IMG);
}

// 绘制音符图片图标
void draw_note_icon_img(LGFX_Sprite* dst, int x, int y, uint16_t color)
{
  (void)color;
  draw_icon_image(dst, x, y, ICON_NOTE_IMG);
}