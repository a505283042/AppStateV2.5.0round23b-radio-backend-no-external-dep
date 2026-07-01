#include "ui/ui_power_prompt.h"

#include "ui/ui.h"
#include "ui/ui_internal.h"
#include "ui/ui_text_utils.h"

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

    tft.fillScreen(TFT_BLACK);
    tft.setFont(&g_font_cjk);
    tft.setTextWrap(false);

    // 简单电源图标
    tft.drawCircle(120, 72, 28, TFT_DARKGREY);
    tft.drawFastVLine(120, 46, 26, TFT_WHITE);
   tft.drawArc(120, 76, 24, 25, 35, 325, TFT_WHITE);

    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    draw_center_text(line1, 128);

    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    draw_center_text(line2, 154);

    ui_draw_unlock();
}