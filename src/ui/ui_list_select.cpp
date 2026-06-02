#include <Arduino.h>
#include <algorithm>
#include "ui/ui_internal.h"
#include "ui/ui_text_utils.h"
#include "ui/ui_list_select_view.h"
#include "storage/storage_catalog_v3.h"
#include "storage/storage_groups_v3.h"
#include "storage/storage_view_v3.h"
#include "radio/radio_catalog.h"

static String truncateByPixel(const String& text, int maxWidth)
{
  String result = text;
  while (result.length() > 0) {
    // 先检查当前字符串是否符合宽度要求
    if (tft.textWidth(result) <= maxWidth) {
      break;
    }
    
    // 按字节长度回退到上一个完整的字符起始位
    int len = result.length();
    // 从后向前找到第一个字符的起始位置
    while (len > 0) {
      unsigned char c = result.charAt(len - 1);
      // UTF-8 字符起始字节的最高两位不是 10
      if ((c & 0xC0) != 0x80) {
        break;
      }
      len--;
    }
    // 如果找到起始位置，截取到该位置
    if (len > 0) {
      result = result.substring(0, len - 1);
    } else {
      // 没有找到有效字符，清空
      result = "";
    }
  }
  return result;
}

// 真正的像素级滚动绘制：完整文本 + 间隙 + 完整文本
static void drawScrollingTextPixel(const String& text,
                                   int text_start_x,
                                   int text_mid_y,
                                   int clip_y,
                                   int clip_h,
                                   int available_width,
                                   int scroll_offset,
                                   uint16_t text_color,
                                   uint16_t bg_color)
{
  extern lgfx::U8g2font g_font_cjk;

  tft.setFont(&g_font_cjk);
  tft.setTextSize(1);
  tft.setTextWrap(false);

  // 先清滚动区域，避免残影
  tft.fillRect(text_start_x, clip_y, available_width, clip_h, bg_color);

  // 只裁名字区域
  tft.setClipRect(text_start_x, clip_y, available_width, clip_h);
  tft.setTextColor(text_color, bg_color);
  tft.setTextDatum(middle_left);

  int x = text_start_x - scroll_offset;
  tft.drawString(text, x, text_mid_y);

  tft.clearClipRect();
  tft.setTextDatum(top_left);
}

// 按字符偏移截取子串，支持UTF-8中文字符
// 参数: text - 原始字符串
//       charOffset - 字符偏移量（不是字节偏移）
// 返回: 从偏移位置开始的子串
static String getSubStrByCharOffset(const String& text, int charOffset)
{
  if (charOffset <= 0) return text;
  
  int char_count = 0;
  int byte_pos = 0;
  while (char_count < charOffset && byte_pos < text.length()) {
    unsigned char c = text.charAt(byte_pos);
    if ((c & 0x80) == 0) {
      byte_pos++;  // ASCII字符，1字节
    } else if ((c & 0xE0) == 0xC0) {
      byte_pos += 2;  // 2字节UTF-8字符
    } else if ((c & 0xF0) == 0xE0) {
      byte_pos += 3;  // 3字节UTF-8字符（中文）
    } else if ((c & 0xF8) == 0xF0) {
      byte_pos += 4;  // 4字节UTF-8字符
    } else {
      byte_pos++;
    }
    char_count++;
  }
  
  if (byte_pos < text.length()) {
    return text.substring(byte_pos);
  }
  return "";
}

// =============================================================================
// 滚动控制结构体
struct ListScrollState {
  enum class Phase : uint8_t {
    HOLD_HEAD,   // 起点停顿
    SCROLLING,   // 向左滚动
    HOLD_TAIL    // 终点停顿
  };

  int scroll_idx = -1;             // 当前滚动项索引
  int scroll_offset = 0;           // 当前滚动偏移（像素）
  uint32_t last_scroll_time = 0;   // 上次真正滚动的时间
  uint32_t phase_start_time = 0;   // 当前阶段开始时间
  Phase phase = Phase::HOLD_HEAD;

  static constexpr uint32_t SCROLL_INTERVAL_MS = 50; // 每 50ms 滚 1px，约 20px/s
  static constexpr int SCROLL_STEP_PX = 1;
  static constexpr uint32_t HOLD_HEAD_MS = 1000;
  static constexpr uint32_t HOLD_TAIL_MS = 1200;

  // 重置滚动状态
  void reset(int new_idx) {
    scroll_idx = new_idx;
    scroll_offset = 0;
    const uint32_t now = millis();
    last_scroll_time = now;
    phase_start_time = now;
    phase = Phase::HOLD_HEAD;
  }

  // 更新滚动偏移
  // 逻辑：头部停顿 -> 单条文本左移 -> 尾部停顿 -> 回到起点
  // 返回: 偏移或阶段是否发生了可见变化，需要重绘
  bool update(int full_text_width, int available_width) {
    const uint32_t now = millis();

    if (full_text_width <= available_width) {
      bool changed = (scroll_offset != 0) || (phase != Phase::HOLD_HEAD);
      scroll_offset = 0;
      phase = Phase::HOLD_HEAD;
      last_scroll_time = now;
      phase_start_time = now;
      return changed;
    }

    const int max_offset = full_text_width - available_width;

    switch (phase) {
      case Phase::HOLD_HEAD:
        if (now - phase_start_time >= HOLD_HEAD_MS) {
          phase = Phase::SCROLLING;
          last_scroll_time = now;
        }
        return false;

      case Phase::SCROLLING: {
        if (now - last_scroll_time < SCROLL_INTERVAL_MS) {
          return false;
        }

        int new_offset = scroll_offset;
        while (now - last_scroll_time >= SCROLL_INTERVAL_MS) {
          last_scroll_time += SCROLL_INTERVAL_MS;
          new_offset += SCROLL_STEP_PX;
        }

        if (new_offset >= max_offset) {
          new_offset = max_offset;
          phase = Phase::HOLD_TAIL;
          phase_start_time = now;
        }

        bool changed = (new_offset != scroll_offset);
        scroll_offset = new_offset;
        return changed;
      }

      case Phase::HOLD_TAIL:
        if (now - phase_start_time >= HOLD_TAIL_MS) {
          scroll_offset = 0;
          phase = Phase::HOLD_HEAD;
          phase_start_time = now;
          last_scroll_time = now;
          return true;
        }
        return false;
    }

    return false;
  }
};

// 列表选择界面静态变量
static int s_last_selected_idx = -1;
static int s_last_start_idx = -1;
static bool s_first_draw = true;
static int last_drawn_selected = -1;
static int last_drawn_offset = -1;
static ListScrollState s_scroll_state; // 滚动状态

// =============================================================================
// 绘制辅助函数

// 绘制列表框架（标题、分隔线、滚动条、底部提示）
// 参数: title - 标题文字
//       start_idx - 当前显示起始索引
//       total - 总项目数
//       visible - 可见项目数
static void drawListFrame(const char* title, int start_idx, int total, int visible)
{
  extern lgfx::U8g2font g_font_cjk;

  // 绘制标题
  tft.setFont(&g_font_cjk);
  tft.setTextSize(1);
  tft.setTextWrap(false);
  tft.setTextColor(TFT_WHITE);
  draw_center_text(title, 25);

  // 绘制分隔线
  tft.drawFastHLine(20, 40, 200, TFT_WHITE);

  // 绘制滚动指示器
  if (total > visible) {
    const int ITEM_HEIGHT = 18;
    const int START_Y = 60;
    int bar_height = visible * ITEM_HEIGHT;
    int bar_y = START_Y - 7;
    int thumb_height = (visible * bar_height) / total;
    if (thumb_height < 10) thumb_height = 10;

    int thumb_y = bar_y + (start_idx * (bar_height - thumb_height)) / (total - visible);
    
    // 确保滑块不超出滚动条范围
    if (thumb_y < bar_y) thumb_y = bar_y;
    if (thumb_y + thumb_height > bar_y + bar_height) thumb_y = bar_y + bar_height - thumb_height;

    tft.drawRect(225, bar_y, 4, bar_height, TFT_DARKGREY);
    tft.fillRect(225, thumb_y, 4, thumb_height, TFT_WHITE);
  }

  // 绘制底部提示（分两行，居中显示）
  tft.setTextColor(TFT_LIGHTGREY);
  draw_center_text("NEXT/PREV:选择 VOL:翻页", 171);
  draw_center_text("PLAY:确认 MODE:取消", 185);
}

// 获取列表行的矩形位置和尺寸
// 参数: list_pos - 列表中的显示位置（0-4，对应屏幕上的5行）
//       row_top - 输出参数，返回行的顶部Y坐标
//       row_h   - 输出参数，返回行的高度
static void getListRowRect(int list_pos, int& row_top, int& row_h)
{
  const int ITEM_HEIGHT = 18;
  const int START_Y = 60;
  int y = START_Y + list_pos * ITEM_HEIGHT;
  row_top = y - ITEM_HEIGHT / 2;
  row_h = ITEM_HEIGHT;
}

// 绘制单个列表项
// =============================================================================
// 绘制单个列表项
// =============================================================================
// 布局说明：
//   ┌─────────────────────────────────────────────────────┐
//   │ [row_x=10]                                          │
//   │   ┌─────────────────────────────────────────────┐   │
//   │   │ 1. 歌手/专辑名称          (N首)              │   │ ← row_top
//   │   │  ↑                          ↑                │   │
//   │   │  list_left_edge            list_right_edge   │   │
//   │   │  (25)                      (210)             │   │
//   │   └─────────────────────────────────────────────┘   │
//   │   ←────────────── row_w=210 ──────────────────→     │
//   └─────────────────────────────────────────────────────┘
//
// 文本布局：
//   [序号.][    名称（滚动区域）    ][ (N首)]
//    ↑     ↑                        ↑       ↑
//   25    text_start_x         text_end_x  210
//
// 参数说明：
//   group        - 列表组数据（包含名称和曲目索引）
//   idx          - 项目索引（用于显示序号，从0开始）
//   list_pos     - 列表中的显示位置（0-4，对应屏幕上的5行）
//   is_selected  - 是否选中（选中项显示黄色+背景条）
//   scroll_offset - 滚动偏移量（像素），仅选中项有效
//   draw_bg      - 是否绘制背景条
//                  true: 正常绘制（选中项蓝色，非选中项黑色清除旧背景）
//                  false: 滚动更新时不重绘背景，避免闪烁
// =============================================================================
static String track_display_name(TrackIndex16 track_idx)
{
  TrackInfo t;
  if (storage_fill_trackinfo_from_v3(storage_catalog_v3(), track_idx, t)) {
    return t.title;
  }
  return String("未知歌曲");
}

static void drawListItem(const PlaylistGroup& group, int idx, int list_pos,
                         bool is_selected, int scroll_offset = 0, bool draw_bg = true)
{
  extern lgfx::U8g2font g_font_cjk;
  tft.setFont(&g_font_cjk);
  tft.setTextSize(1);
  tft.setTextWrap(false);

  const int ITEM_HEIGHT = 18;
  const int START_Y = 60;
  int y = START_Y + list_pos * ITEM_HEIGHT;

  const int row_x = 10;
  const int row_w = 210;
  const int row_h = ITEM_HEIGHT;
  const int row_r = 5;

  int row_top = y - row_h / 2;
  int row_mid_y = row_top + row_h / 2;
  int clip_y = row_top;
  int clip_h = row_h;

  if (draw_bg) {
    if (is_selected) {
      tft.fillRoundRect(row_x, row_top, row_w, row_h, row_r, 0x4208);
    } else {
      tft.fillRoundRect(row_x, row_top, row_w, row_h, row_r, TFT_BLACK);
    }
  }

  const MusicCatalogV3& cat = storage_catalog_v3();
  String name = playlist_group_display_string(cat, group);

  String prefix = String(idx + 1) + ". ";
  int prefix_width = tft.textWidth(prefix);

  String count = "(" + String(group.track_indices.size()) + "首)";
  int count_width = tft.textWidth(count);

  int list_left_edge = 25;
  int list_right_edge = 210;

  int text_start_x = list_left_edge + prefix_width;
  int text_end_x = list_right_edge - count_width;
  int available_width = text_end_x - text_start_x;

  int name_width = tft.textWidth(name);
  bool need_scroll = is_selected && (name_width > available_width);

  if (need_scroll) {
    // 序号
    tft.setTextColor(TFT_YELLOW, 0x4208);
    tft.setTextDatum(middle_left);
    tft.drawString(prefix, list_left_edge, row_mid_y);

    // 名字滚动
    drawScrollingTextPixel(name,
                           text_start_x,
                           row_mid_y,
                           clip_y,
                           clip_h,
                           available_width,
                           scroll_offset,
                           TFT_YELLOW,
                           0x4208);

    // 数量
    tft.setTextColor(TFT_LIGHTGREY, 0x4208);
    tft.setTextDatum(middle_right);
    tft.drawString(count, list_right_edge, row_mid_y);
  } else {
    String display_name = name;

    if (name_width > available_width) {
      display_name = truncateByPixel(name, available_width);
      if (display_name.length() < name.length()) {
        int ellipsis_width = tft.textWidth("...");
        if (tft.textWidth(display_name) + ellipsis_width <= available_width) {
          display_name += "...";
        } else {
          display_name = truncateByPixel(display_name, available_width - ellipsis_width);
          display_name += "...";
        }
      }
    }

    tft.setTextColor(is_selected ? TFT_YELLOW : TFT_WHITE,
                     is_selected ? 0x4208 : TFT_BLACK);
    tft.setTextDatum(middle_left);
    tft.drawString(prefix + display_name, list_left_edge, row_mid_y);

    tft.setTextColor(TFT_LIGHTGREY, is_selected ? 0x4208 : TFT_BLACK);
    tft.setTextDatum(middle_right);
    tft.drawString(count, list_right_edge, row_mid_y);
  }

  tft.setTextDatum(top_left);
}

static void drawRadioItem(const RadioItem& item, int idx, int list_pos,
                          bool is_selected, int scroll_offset = 0, bool draw_bg = true)
{
  extern lgfx::U8g2font g_font_cjk;
  tft.setFont(&g_font_cjk);
  tft.setTextSize(1);
  tft.setTextWrap(false);

  const int ITEM_HEIGHT = 18;
  const int START_Y = 60;
  int y = START_Y + list_pos * ITEM_HEIGHT;

  const int row_x = 10;
  const int row_w = 210;
  const int row_h = ITEM_HEIGHT;
  const int row_r = 5;

  int row_top = y - row_h / 2;
  int row_mid_y = row_top + row_h / 2;
  int clip_y = row_top;
  int clip_h = row_h;

  if (draw_bg) {
    if (is_selected) {
      tft.fillRoundRect(row_x, row_top, row_w, row_h, row_r, 0x4208);
    } else {
      tft.fillRoundRect(row_x, row_top, row_w, row_h, row_r, TFT_BLACK);
    }
  }

  String name = item.name;
  String prefix = String(idx + 1) + ". ";
  int prefix_width = tft.textWidth(prefix);

  String region = item.region;
  int region_width = tft.textWidth(region);

  int list_left_edge = 25;
  int list_right_edge = 210;

  int text_start_x = list_left_edge + prefix_width;
  int text_end_x = list_right_edge - region_width;
  int available_width = text_end_x - text_start_x;

  int name_width = tft.textWidth(name);
  bool need_scroll = is_selected && (name_width > available_width);

  if (need_scroll) {
    tft.setTextColor(TFT_YELLOW, 0x4208);
    tft.setTextDatum(middle_left);
    tft.drawString(prefix, list_left_edge, row_mid_y);

    drawScrollingTextPixel(name,
                           text_start_x,
                           row_mid_y,
                           clip_y,
                           clip_h,
                           available_width,
                           scroll_offset,
                           TFT_YELLOW,
                           0x4208);

    tft.setTextColor(TFT_LIGHTGREY, 0x4208);
    tft.setTextDatum(middle_right);
    tft.drawString(region, list_right_edge, row_mid_y);
  } else {
    String display_name = name;

    if (name_width > available_width) {
      display_name = truncateByPixel(name, available_width);
      if (display_name.length() < name.length()) {
        int ellipsis_width = tft.textWidth("...");
        if (tft.textWidth(display_name) + ellipsis_width <= available_width) {
          display_name += "...";
        } else {
          display_name = truncateByPixel(display_name, available_width - ellipsis_width);
          display_name += "...";
        }
      }
    }

    tft.setTextColor(is_selected ? TFT_YELLOW : TFT_WHITE,
                     is_selected ? 0x4208 : TFT_BLACK);
    tft.setTextDatum(middle_left);
    tft.drawString(prefix + display_name, list_left_edge, row_mid_y);

    tft.setTextColor(TFT_LIGHTGREY, is_selected ? 0x4208 : TFT_BLACK);
    tft.setTextDatum(middle_right);
    tft.drawString(region, list_right_edge, row_mid_y);
  }

  tft.setTextDatum(top_left);
}



static void drawTrackItem(TrackIndex16 track_idx, int idx, int list_pos,
                          bool is_selected, int scroll_offset = 0, bool draw_bg = true)
{
  extern lgfx::U8g2font g_font_cjk;
  tft.setFont(&g_font_cjk);
  tft.setTextSize(1);
  tft.setTextWrap(false);

  const int ITEM_HEIGHT = 18;
  const int START_Y = 60;
  int y = START_Y + list_pos * ITEM_HEIGHT;

  const int row_x = 10;
  const int row_w = 210;
  const int row_h = ITEM_HEIGHT;
  const int row_r = 5;

  int row_top = y - row_h / 2;
  int row_mid_y = row_top + row_h / 2;
  int clip_y = row_top;
  int clip_h = row_h;

  if (draw_bg) {
    if (is_selected) {
      tft.fillRoundRect(row_x, row_top, row_w, row_h, row_r, 0x4208);
    } else {
      tft.fillRoundRect(row_x, row_top, row_w, row_h, row_r, TFT_BLACK);
    }
  }

  String name = track_display_name(track_idx);

  String prefix = String(idx + 1) + ". ";
  int prefix_width = tft.textWidth(prefix);

  const int list_left_edge = 25;
  const int list_right_edge = 210;

  int text_start_x = list_left_edge + prefix_width;
  int available_width = list_right_edge - text_start_x;

  int name_width = tft.textWidth(name);
  bool need_scroll = is_selected && (name_width > available_width);

  if (need_scroll) {
    tft.setTextColor(TFT_YELLOW, 0x4208);
    tft.setTextDatum(middle_left);
    tft.drawString(prefix, list_left_edge, row_mid_y);

    drawScrollingTextPixel(name,
                           text_start_x,
                           row_mid_y,
                           clip_y,
                           clip_h,
                           available_width,
                           scroll_offset,
                           TFT_YELLOW,
                           0x4208);
  } else {
    String display_name = name;

    if (name_width > available_width) {
      display_name = truncateByPixel(name, available_width);
      if (display_name.length() < name.length()) {
        int ellipsis_width = tft.textWidth("...");
        if (tft.textWidth(display_name) + ellipsis_width <= available_width) {
          display_name += "...";
        } else {
          display_name = truncateByPixel(display_name, available_width - ellipsis_width);
          display_name += "...";
        }
      }
    }

    tft.setTextColor(is_selected ? TFT_YELLOW : TFT_WHITE,
                     is_selected ? 0x4208 : TFT_BLACK);
    tft.setTextDatum(middle_left);
    tft.drawString(prefix + display_name, list_left_edge, row_mid_y);
  }

  tft.setTextDatum(top_left);
}

// =============================================================================
// 绘制列表选择界面
// 参数: groups - 列表组数据
//       selected_idx - 当前选中的索引
//       title - 界面标题（"选择歌手"或"选择专辑"）
// 注意: 调用此函数前必须已获取 ui_lock()
void ui_draw_list_select(const std::vector<PlaylistGroup>& groups, int selected_idx, const char* title)
{
  if (groups.empty()) return;

  extern lgfx::U8g2font g_font_cjk;
  tft.setFont(&g_font_cjk);
  tft.setTextSize(1);
  tft.setTextWrap(false);

  // --- 基础参数配置 ---
  const int ITEMS_VISIBLE = 5;
  const int ITEM_HEIGHT = 18;
  const int START_Y = 60;
  const uint16_t COLOR_SEL_BG = 0x4208;  // 选中行背景色（深蓝色）

  int total = (int)groups.size();

  // 1. 检查选中项是否改变，改变则立即重置滚动状态
  if (selected_idx != s_scroll_state.scroll_idx) {
    s_scroll_state.reset(selected_idx);
  }

  // 2. 分页逻辑计算
  int current_page = selected_idx / ITEMS_VISIBLE;
  int start_idx = current_page * ITEMS_VISIBLE;
  int end_idx = std::min(start_idx + ITEMS_VISIBLE, total);

  // 3. 判定刷新类型
  bool is_page_changed = (start_idx != s_last_start_idx);
  bool is_selection_changed = (selected_idx != s_last_selected_idx);

  // 更新滚动偏移量（每帧调用，内部自行处理停顿和位移节奏）
  bool is_offset_changed = false;
  {
    // 计算当前选中项是否真的需要滚动
    String prefix = String(selected_idx + 1) + ". ";
    int prefix_width = tft.textWidth(prefix);

    String count = "(" + String(groups[selected_idx].track_indices.size()) + "首)";
    int count_width = tft.textWidth(count);

    // 和 drawListItem() 保持完全一致
    const int list_left_edge = 25;
    const int list_right_edge = 210;
    int text_start_x = list_left_edge + prefix_width;
    int text_end_x   = list_right_edge - count_width;
    int available_width = text_end_x - text_start_x;

    // 构建显示名称（与 drawListItem 中一致）
    const MusicCatalogV3& cat = storage_catalog_v3();
    String name = playlist_group_display_string(cat, groups[selected_idx]);

    int full_width = tft.textWidth(name);

    // 更新偏移量
    is_offset_changed = s_scroll_state.update(full_width, available_width);
  }

  // 防止频繁重绘检查
  if (!s_first_draw && !is_page_changed && !is_selection_changed && !is_offset_changed &&
      selected_idx == last_drawn_selected && s_scroll_state.scroll_offset == last_drawn_offset) {
    // 无变化，直接返回
    return;
  }

  // --- 绘制逻辑开始 ---

  // 情况 A: 换页或首次进入 -> 全刷
  if (s_first_draw || is_page_changed) {
    tft.fillScreen(TFT_BLACK);
    drawListFrame(title, start_idx, total, ITEMS_VISIBLE);

    for (int i = start_idx; i < end_idx; i++) {
      int list_pos = i - start_idx;
      bool is_selected = (i == selected_idx);
      drawListItem(groups[i], i, list_pos, is_selected,
                   is_selected ? s_scroll_state.scroll_offset : 0, true);
    }
  }
  // 情况 B: 同一页内切换选中行 -> 局部刷两行
  else if (is_selection_changed) {
    // 1. 恢复旧行：清除高亮并重画普通文字
    if (s_last_selected_idx >= start_idx && s_last_selected_idx < end_idx) {
      int old_pos = s_last_selected_idx - start_idx;
      int old_row_top, old_row_h;
      getListRowRect(old_pos, old_row_top, old_row_h);
      tft.fillRoundRect(10, old_row_top, 210, old_row_h, 5, TFT_BLACK);
      drawListItem(groups[s_last_selected_idx], s_last_selected_idx, old_pos, false, 0, false);
    }
    // 2. 绘制新行：画高亮背景并重画文字
    int new_pos = selected_idx - start_idx;
    drawListItem(groups[selected_idx], selected_idx, new_pos, true, 0, true);
  }
  // 情况 C: 仅滚动偏移改变 -> 局部刷文字区
  else if (is_offset_changed) {
    int list_pos = selected_idx - start_idx;

    // 不要整行重刷背景
    // drawScrollingTextPixel() 内部已经会清名字滚动区
    drawListItem(groups[selected_idx], selected_idx, list_pos, true, s_scroll_state.scroll_offset, false);
  }

  // 记录状态供下一帧对比
  s_last_selected_idx = selected_idx;
  s_last_start_idx = start_idx;
  last_drawn_selected = selected_idx;
  last_drawn_offset = s_scroll_state.scroll_offset;
  s_first_draw = false;
}

void ui_draw_radio_select(const std::vector<RadioItem>& radios, int selected_idx, const char* title)
{
  if (radios.empty()) return;

  extern lgfx::U8g2font g_font_cjk;
  tft.setFont(&g_font_cjk);
  tft.setTextSize(1);
  tft.setTextWrap(false);

  const int ITEMS_VISIBLE = 5;
  int total = (int)radios.size();

  if (selected_idx != s_scroll_state.scroll_idx) {
    s_scroll_state.reset(selected_idx);
  }

  int current_page = selected_idx / ITEMS_VISIBLE;
  int start_idx = current_page * ITEMS_VISIBLE;
  int end_idx = std::min(start_idx + ITEMS_VISIBLE, total);

  bool is_page_changed = (start_idx != s_last_start_idx);
  bool is_selection_changed = (selected_idx != s_last_selected_idx);

  bool is_offset_changed = false;
  {
    String prefix = String(selected_idx + 1) + ". ";
    int prefix_width = tft.textWidth(prefix);

    String region = radios[selected_idx].region;
    int region_width = tft.textWidth(region);

    const int list_left_edge = 25;
    const int list_right_edge = 210;
    int text_start_x = list_left_edge + prefix_width;
    int text_end_x   = list_right_edge - region_width;
    int available_width = text_end_x - text_start_x;

    String name = radios[selected_idx].name;
    int full_width = tft.textWidth(name);

    is_offset_changed = s_scroll_state.update(full_width, available_width);
  }

  if (!s_first_draw && !is_page_changed && !is_selection_changed && !is_offset_changed &&
      selected_idx == last_drawn_selected && s_scroll_state.scroll_offset == last_drawn_offset) {
    return;
  }

  if (s_first_draw || is_page_changed) {
    tft.fillScreen(TFT_BLACK);
    drawListFrame(title, start_idx, total, ITEMS_VISIBLE);

    for (int i = start_idx; i < end_idx; i++) {
      int list_pos = i - start_idx;
      bool is_selected = (i == selected_idx);
      drawRadioItem(radios[i], i, list_pos, is_selected,
                    is_selected ? s_scroll_state.scroll_offset : 0, true);
    }
  }
  else if (is_selection_changed) {
    if (s_last_selected_idx >= start_idx && s_last_selected_idx < end_idx) {
      int old_pos = s_last_selected_idx - start_idx;
      int old_row_top, old_row_h;
      getListRowRect(old_pos, old_row_top, old_row_h);
      tft.fillRoundRect(10, old_row_top, 210, old_row_h, 5, TFT_BLACK);
      drawRadioItem(radios[s_last_selected_idx], s_last_selected_idx, old_pos, false, 0, false);
    }

    int new_pos = selected_idx - start_idx;
    drawRadioItem(radios[selected_idx], selected_idx, new_pos, true, 0, true);
  }
  else if (is_offset_changed) {
    int list_pos = selected_idx - start_idx;
    drawRadioItem(radios[selected_idx], selected_idx, list_pos, true, s_scroll_state.scroll_offset, false);
  }

  s_last_selected_idx = selected_idx;
  s_last_start_idx = start_idx;
  last_drawn_selected = selected_idx;
  last_drawn_offset = s_scroll_state.scroll_offset;
  s_first_draw = false;
}

void ui_draw_track_select(const std::vector<TrackIndex16>& tracks, int selected_idx, const char* title)
{
  if (tracks.empty()) return;

  extern lgfx::U8g2font g_font_cjk;
  tft.setFont(&g_font_cjk);
  tft.setTextSize(1);
  tft.setTextWrap(false);

  const int ITEMS_VISIBLE = 5;
  int total = (int)tracks.size();

  if (selected_idx != s_scroll_state.scroll_idx) {
    s_scroll_state.reset(selected_idx);
  }

  int current_page = selected_idx / ITEMS_VISIBLE;
  int start_idx = current_page * ITEMS_VISIBLE;
  int end_idx = std::min(start_idx + ITEMS_VISIBLE, total);

  bool is_page_changed = (start_idx != s_last_start_idx);
  bool is_selection_changed = (selected_idx != s_last_selected_idx);

  bool is_offset_changed = false;
  {
    String prefix = String(selected_idx + 1) + ". ";
    int prefix_width = tft.textWidth(prefix);

    const int list_left_edge = 25;
    const int list_right_edge = 210;
    int text_start_x = list_left_edge + prefix_width;
    int available_width = list_right_edge - text_start_x;

    String name = track_display_name(tracks[selected_idx]);
    int full_width = tft.textWidth(name);

    is_offset_changed = s_scroll_state.update(full_width, available_width);
  }

  if (!s_first_draw && !is_page_changed && !is_selection_changed && !is_offset_changed &&
      selected_idx == last_drawn_selected && s_scroll_state.scroll_offset == last_drawn_offset) {
    return;
  }

  if (s_first_draw || is_page_changed) {
    tft.fillScreen(TFT_BLACK);
    drawListFrame(title, start_idx, total, ITEMS_VISIBLE);

    for (int i = start_idx; i < end_idx; i++) {
      int list_pos = i - start_idx;
      bool is_selected = (i == selected_idx);
      drawTrackItem(tracks[i], i, list_pos, is_selected,
                    is_selected ? s_scroll_state.scroll_offset : 0, true);
    }
  }
  else if (is_selection_changed) {
    if (s_last_selected_idx >= start_idx && s_last_selected_idx < end_idx) {
      int old_pos = s_last_selected_idx - start_idx;
      int old_row_top, old_row_h;
      getListRowRect(old_pos, old_row_top, old_row_h);
      tft.fillRoundRect(10, old_row_top, 210, old_row_h, 5, TFT_BLACK);
      drawTrackItem(tracks[s_last_selected_idx], s_last_selected_idx, old_pos, false, 0, false);
    }

    int new_pos = selected_idx - start_idx;
    drawTrackItem(tracks[selected_idx], selected_idx, new_pos, true, 0, true);
  }
  else if (is_offset_changed) {
    int list_pos = selected_idx - start_idx;
    drawTrackItem(tracks[selected_idx], selected_idx, list_pos, true, s_scroll_state.scroll_offset, false);
  }

  s_last_selected_idx = selected_idx;
  s_last_start_idx = start_idx;
  last_drawn_selected = selected_idx;
  last_drawn_offset = s_scroll_state.scroll_offset;
  s_first_draw = false;
}

// 清除列表选择界面
void ui_clear_list_select()
{
  ui_lock();

  // 这里不再立刻清黑屏，只重置列表状态。
  // 这样退出列表或确认切歌时，不会把后续准备过程完整暴露成黑屏。
  s_first_draw = true;
  s_last_selected_idx = -1;
  s_last_start_idx = -1;
  last_drawn_selected = -1;
  last_drawn_offset = -1;
  s_scroll_state.reset(-1); // 重置滚动状态

  // 清除裁剪区域，避免异常退出时裁剪区域未被清除
  tft.clearClipRect();

  ui_unlock();

  if (s_ui_task) {
    xTaskNotifyGive(s_ui_task);
  }
}
