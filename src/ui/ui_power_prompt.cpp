#include "ui/ui_power_prompt.h"

#include "ui/ui.h"
#include "ui/ui_internal.h"
#include "ui/ui_text_utils.h"

#include <string.h>

namespace {

static constexpr uint16_t COLOR_BG_TOP = 0x0208;
static constexpr uint16_t COLOR_BG_BOTTOM = 0x0000;
static constexpr uint16_t COLOR_CARD = 0x18E3;
static constexpr uint16_t COLOR_CARD_EDGE = 0x39E7;
static constexpr uint16_t COLOR_CARD_SHADOW = 0x0841;
static constexpr uint16_t COLOR_ACCENT = 0x06FF;
static constexpr uint16_t COLOR_ACCENT_DIM = 0x0398;
static constexpr uint16_t COLOR_OK = 0x07E0;
static constexpr uint16_t COLOR_WARN = 0xFD20;
static constexpr uint16_t COLOR_MUTED = 0xBDF7;
static constexpr uint16_t COLOR_DOT_DIM = 0x3186;

static bool contains_text(const char* s, const char* needle)
{
    return s && needle && strstr(s, needle) != nullptr;
}

static bool is_success_stage(const char* line1)
{
    return contains_text(line1, "完成");
}

static bool is_error_stage(const char* line1)
{
    return contains_text(line1, "失败");
}

static uint16_t stage_accent_color(const char* line1)
{
    if (is_error_stage(line1)) {
        return COLOR_WARN;
    }
    if (is_success_stage(line1)) {
        return COLOR_OK;
    }
    return COLOR_ACCENT;
}

static void draw_soft_background()
{
    // 轻量竖向暗色渐变，不使用大 Sprite，避免关机阶段额外内存压力。
    for (int y = 0; y < COVER_SIZE; ++y) {
        const uint8_t step = (uint8_t)(y >> 4);
        const uint16_t c = (y < 120) ? (uint16_t)(COLOR_BG_TOP + step) : COLOR_BG_BOTTOM;
        tft.drawFastHLine(0, y, COVER_SIZE, c);
    }

    tft.drawCircle(120, 120, 118, 0x1082);
    tft.drawCircle(120, 120, 106, 0x0841);
    tft.drawCircle(120, 120, 84, 0x0008);
}

static void draw_power_icon(int cx, int cy, uint16_t accent, bool done, bool warn)
{
    tft.fillCircle(cx, cy, 34, 0x0000);
    tft.drawCircle(cx, cy, 34, COLOR_CARD_EDGE);
    tft.drawCircle(cx, cy, 31, accent);

    if (done) {
        // 勾选图标
        tft.drawLine(cx - 13, cy, cx - 4, cy + 10, TFT_WHITE);
        tft.drawLine(cx - 12, cy + 1, cx - 4, cy + 9, TFT_WHITE);
        tft.drawLine(cx - 4, cy + 10, cx + 15, cy - 12, TFT_WHITE);
        tft.drawLine(cx - 3, cy + 10, cx + 16, cy - 11, TFT_WHITE);
        return;
    }

    if (warn) {
        tft.drawFastVLine(cx, cy - 14, 22, TFT_WHITE);
        tft.drawFastVLine(cx + 1, cy - 14, 22, TFT_WHITE);
        tft.fillCircle(cx, cy + 15, 2, TFT_WHITE);
        return;
    }

    // 电源图标
    tft.drawFastVLine(cx, cy - 26, 24, TFT_WHITE);
    tft.drawFastVLine(cx + 1, cy - 26, 24, TFT_WHITE);
    // LovyanGFX 的 0° 在右侧。原来 35°..325° 会把缺口留在右边；
    // 分成两段绘制，把缺口旋转到正上方并与电源竖线对齐。
    tft.drawArc(cx, cy + 3, 24, 25, 305, 359, TFT_WHITE);
    tft.drawArc(cx, cy + 3, 24, 25, 0, 235, TFT_WHITE);
    tft.drawArc(cx, cy + 3, 21, 22, 312, 359, accent);
    tft.drawArc(cx, cy + 3, 21, 22, 0, 228, accent);
}

static void draw_status_dots(uint16_t accent, bool done, bool warn)
{
    const int y = 178;
    const int r = 3;
    const int x0 = 105;
    const int gap = 15;

    if (done || warn) {
        const uint16_t c = done ? COLOR_OK : COLOR_WARN;
        tft.fillCircle(x0, y, r, c);
        tft.fillCircle(x0 + gap, y, r, c);
        tft.fillCircle(x0 + gap * 2, y, r, c);
        return;
    }

    const uint8_t phase = (uint8_t)((millis() / 260) % 3);
    for (uint8_t i = 0; i < 3; ++i) {
        tft.fillCircle(x0 + gap * i, y, r, i == phase ? accent : COLOR_DOT_DIM);
    }
}

static void draw_footer_hint(bool done, bool warn)
{
    const char* hint = done ? "正在断电" : (warn ? "部分未保存" : "请稍候");
    const uint16_t color = done ? COLOR_OK : (warn ? COLOR_WARN : COLOR_MUTED);

    tft.setTextSize(1);
    tft.setTextColor(color, COLOR_BG_BOTTOM);
    draw_center_text(hint, 211);
}

} // namespace

void ui_power_show_shutdown_stage(const char* line1, const char* line2)
{
    if (!line1) {
        line1 = "";
    }

    if (!line2) {
        line2 = "";
    }

    // 关机提示期间暂停 UiTask 自动刷新，避免提示被播放器页面覆盖。
    ui_hold_render(true);

    ui_draw_lock();

    const bool done = is_success_stage(line1);
    const bool warn = is_error_stage(line1);
    const uint16_t accent = stage_accent_color(line1);

    draw_soft_background();

    // 主卡片：留出圆屏边缘，避免文字贴边。
    tft.fillRoundRect(24, 45, 192, 148, 18, COLOR_CARD_SHADOW);
    tft.fillRoundRect(20, 39, 200, 148, 18, COLOR_CARD);
    tft.drawRoundRect(20, 39, 200, 148, 18, COLOR_CARD_EDGE);
    tft.drawFastHLine(42, 91, 156, COLOR_ACCENT_DIM);

    draw_power_icon(120, 70, accent, done, warn);

    tft.setFont(&g_font_cjk);
    tft.setTextWrap(false);
    tft.setTextSize(1);

    tft.setTextColor(TFT_WHITE, COLOR_CARD);
    draw_center_text(line1, 124);

    tft.setTextColor(COLOR_MUTED, COLOR_CARD);
    draw_center_text(line2, 150);

    draw_status_dots(accent, done, warn);
    draw_footer_hint(done, warn);

    ui_draw_unlock();
}
