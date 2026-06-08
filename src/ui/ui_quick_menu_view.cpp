#include <Arduino.h>

#include "menu/quick_menu.h"
#include "ui/ui_internal.h"
#include "ui/ui_text_utils.h"

namespace {

static constexpr int TITLE_Y = 22;
static constexpr int TITLE_LINE_Y = 40;

static constexpr int ROW_START_Y = 60;
static constexpr int ROW_H = 22;
static constexpr int ROW_COUNT = 5;

static constexpr int ROW_X = 16;
static constexpr int ROW_W = 208;
static constexpr int ROW_R = 6;

static constexpr int LABEL_X = 28;
static constexpr int VALUE_X = 210;

static constexpr uint16_t COLOR_BG = TFT_BLACK;
static constexpr uint16_t COLOR_TEXT = TFT_WHITE;
static constexpr uint16_t COLOR_DIM = TFT_LIGHTGREY;
static constexpr uint16_t COLOR_DISABLED = TFT_DARKGREY;
static constexpr uint16_t COLOR_SELECTED_BG = 0x4208;
static constexpr uint16_t COLOR_SELECTED_TEXT = TFT_YELLOW;

static bool s_first_draw = true;
static QuickMenuPage s_last_page = QuickMenuPage::Root;
static int s_last_start_idx = -1;
static int s_last_selected_idx = -1;

String clip_utf8_for_tft(const String& text, int max_w)
{
    if (text.length() == 0) {
        return text;
    }

    if (tft.textWidth(text) <= max_w) {
        return text;
    }

    String out = text;
    const String ellipsis = "...";

    while (out.length() > 0 && tft.textWidth(out + ellipsis) > max_w) {
        int len = out.length();

        // 回退到 UTF-8 字符边界，避免把中文截坏。
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

int calc_page_start_index(int selected, int total)
{
    if (total <= ROW_COUNT) {
        return 0;
    }

    if (selected < 0) {
        selected = 0;
    }

    if (selected >= total) {
        selected = total - 1;
    }

    // 参照原来的列表：5 行一页，不做居中滚动。
    const int current_page = selected / ROW_COUNT;
    return current_page * ROW_COUNT;
}

void get_menu_row_rect(int row, int& row_top, int& row_h)
{
    const int row_y = ROW_START_Y + row * ROW_H;
    row_top = row_y - ROW_H / 2;
    row_h = ROW_H - 2;
}

void draw_menu_header(const char* title)
{
    tft.setFont(&g_font_cjk);
    tft.setTextSize(1);
    tft.setTextWrap(false);

    tft.setTextColor(COLOR_TEXT, COLOR_BG);
    draw_center_text(title, TITLE_Y);

    tft.drawFastHLine(28, TITLE_LINE_Y, 184, COLOR_DIM);
}

void draw_scroll_bar(int start_idx, int total)
{
    if (total <= ROW_COUNT) {
        return;
    }

    const int bar_x = 225;
    const int bar_y = ROW_START_Y - 10;
    const int bar_h = ROW_COUNT * ROW_H;
    const int bar_w = 4;

    int thumb_h = (ROW_COUNT * bar_h) / total;
    if (thumb_h < 10) {
        thumb_h = 10;
    }

    const int max_start = total - ROW_COUNT;
    int thumb_y = bar_y + (start_idx * (bar_h - thumb_h)) / max_start;

    if (thumb_y < bar_y) {
        thumb_y = bar_y;
    }

    if (thumb_y + thumb_h > bar_y + bar_h) {
        thumb_y = bar_y + bar_h - thumb_h;
    }

    tft.drawRect(bar_x, bar_y, bar_w, bar_h, TFT_DARKGREY);
    tft.fillRect(bar_x, thumb_y, bar_w, thumb_h, COLOR_DIM);
}

void draw_footer()
{
    tft.setFont(&g_font_cjk);
    tft.setTextSize(1);
    tft.setTextWrap(false);

    tft.setTextColor(COLOR_DIM, COLOR_BG);
    draw_center_text("旋钮选择  按下确认", 178);
    draw_center_text("MODE 返回/退出", 193);
}

void draw_menu_frame(const char* title, int start_idx, int total)
{
    tft.fillScreen(COLOR_BG);
    draw_menu_header(title);
    draw_scroll_bar(start_idx, total);
    draw_footer();
}

void draw_menu_row(const QuickMenuItemView& item, int row, bool draw_bg)
{
    const int row_y = ROW_START_Y + row * ROW_H;

    int row_top = 0;
    int row_h = 0;
    get_menu_row_rect(row, row_top, row_h);

    const uint16_t bg = item.selected ? COLOR_SELECTED_BG : COLOR_BG;
    const uint16_t label_color = item.enabled
        ? (item.selected ? COLOR_SELECTED_TEXT : COLOR_TEXT)
        : COLOR_DISABLED;
    const uint16_t value_color = item.enabled
        ? (item.selected ? COLOR_SELECTED_TEXT : COLOR_DIM)
        : COLOR_DISABLED;

    if (draw_bg) {
        tft.fillRoundRect(ROW_X, row_top, ROW_W, row_h, ROW_R, bg);
    }

    tft.setFont(&g_font_cjk);
    tft.setTextSize(1);
    tft.setTextWrap(false);

    String label = item.label ? String(item.label) : String("");
    String value = item.value ? String(item.value) : String("");

    if (item.placeholder && value.length() == 0) {
        value = "占位";
    }

    const int value_w = value.length() > 0 ? tft.textWidth(value) : 0;

    int label_max_w = 160;
    if (value_w > 0) {
        label_max_w = VALUE_X - LABEL_X - value_w - 10;
        if (label_max_w < 60) {
            label_max_w = 60;
        }
    }

    label = clip_utf8_for_tft(label, label_max_w);

    tft.setTextDatum(middle_left);
    tft.setTextColor(label_color, bg);
    tft.drawString(label, LABEL_X, row_y);

    if (value.length() > 0) {
        value = clip_utf8_for_tft(value, 70);

        tft.setTextDatum(middle_right);
        tft.setTextColor(value_color, bg);
        tft.drawString(value, VALUE_X, row_y);
    }

    tft.setTextDatum(top_left);
}

void draw_visible_rows(int start_idx, int total)
{
    for (int row = 0; row < ROW_COUNT; ++row) {
        const int item_idx = start_idx + row;
        if (item_idx >= total) {
            continue;
        }

        QuickMenuItemView item;
        if (!quick_menu_get_item_view(item_idx, item)) {
            continue;
        }

        draw_menu_row(item, row, true);
    }
}

bool get_row_for_item(int item_idx, int start_idx, int total, int& row)
{
    if (item_idx < start_idx) {
        return false;
    }

    if (item_idx >= total) {
        return false;
    }

    row = item_idx - start_idx;
    return row >= 0 && row < ROW_COUNT;
}

} // namespace

void ui_quick_menu_view_reset()
{
    s_first_draw = true;
    s_last_page = QuickMenuPage::Root;
    s_last_start_idx = -1;
    s_last_selected_idx = -1;
}

void ui_draw_quick_menu()
{
    const QuickMenuPage page = quick_menu_get_page();
    const char* title = quick_menu_get_page_title();

    const int total = quick_menu_get_item_count();
    if (total <= 0) {
        return;
    }

    const int selected = quick_menu_get_selected_index();
    const int start_idx = calc_page_start_index(selected, total);

    const bool page_changed = page != s_last_page;
    const bool start_changed = start_idx != s_last_start_idx;
    const bool selection_changed = selected != s_last_selected_idx;

    // 没有任何变化，直接返回。
    if (!s_first_draw && !page_changed && !start_changed && !selection_changed) {
        return;
    }

    // 情况 A：首次进入、切换页面、翻页，整屏重画。
    if (s_first_draw || page_changed || start_changed) {
        draw_menu_frame(title, start_idx, total);
        draw_visible_rows(start_idx, total);
    }
    // 情况 B：同一页内上下移动，只重画旧行和新行。
    else if (selection_changed) {
        int old_row = -1;
        if (get_row_for_item(s_last_selected_idx, start_idx, total, old_row)) {
            QuickMenuItemView old_item;
            if (quick_menu_get_item_view(s_last_selected_idx, old_item)) {
                old_item.selected = false;
                draw_menu_row(old_item, old_row, true);
            }
        }

        int new_row = -1;
        if (get_row_for_item(selected, start_idx, total, new_row)) {
            QuickMenuItemView new_item;
            if (quick_menu_get_item_view(selected, new_item)) {
                new_item.selected = true;
                draw_menu_row(new_item, new_row, true);
            }
        }
    }

    s_first_draw = false;
    s_last_page = page;
    s_last_start_idx = start_idx;
    s_last_selected_idx = selected;
}