#include <Arduino.h>
#include "ui/ui_internal.h"
#include "ui/ui_text_utils.h"
#include "utils/log.h"
#undef LOG_TAG
#define LOG_TAG "UI"

void ui_show_message(const char* msg)
{
  if (!msg) msg = "";
  LOGI("[UI] message: %s", msg);

  ui_draw_lock();
  // 底部提示（对当前全屏封面 UI 安全）
  tft.fillRect(0, 200, 240, 40, TFT_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  draw_center_text(msg, 220);
  ui_draw_unlock();
  ui_request_refresh();
}

void ui_enter_boot(void)
{
  ui_draw_lock();
  ui_set_screen(UI_SCREEN_BOOT);
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  draw_center_text("BOOT", 90);
  tft.setTextSize(1);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  draw_center_text("Mount SD / Scan MUSIC", 130);
  // 离开播放器界面，复位清屏标志
  s_screen_cleared = false;
  ui_draw_unlock();
  ui_request_refresh();
}

void ui_enter_player(void)
{
  ui_lock();
  ui_set_screen(UI_SCREEN_PLAYER);
  
  // 记录进入播放器界面的时间
  s_player_enter_time = millis();
  
  // ✅ 关键：先禁止 UiTask 使用旧封面刷屏，避免闪一下上一首
  s_coverSprReady = false;
  s_src = nullptr;
  s_angle_deg = 0.0f;
  s_rot_last_ms = 0;
  s_rotate_wait_audio_start = false;
  s_rotate_wait_prefetch_done = false;

  // 重置进度（新进入播放器/切歌时由播放层重新喂入）
  s_ui_play_ms  = 0;
  s_ui_total_ms = 0;

  // 重置清屏标志，确保下次渲染时清屏
  s_screen_cleared = false;

  // 延迟清屏：保持启动界面直到音乐加载完成
  // tft.fillScreen(TFT_BLACK);

  s_angle_deg = 0.0f;
  s_rot_last_ms = millis();
  ui_unlock();
  ui_request_refresh();
}

void ui_show_player_loading(const char* title, const char* artist)
{
  ui_draw_lock();
  ui_set_screen(UI_SCREEN_PLAYER);

  // 让 UiTask 暂时不要把旧封面重新推上来
  s_coverSprReady = false;
  s_src = nullptr;
  s_angle_deg = 0.0f;
  s_rot_last_ms = millis();
  s_rotate_wait_audio_start = false;
  s_rotate_wait_prefetch_done = false;

  s_ui_play_ms  = 0;
  s_ui_total_ms = 0;

  tft.fillScreen(TFT_BLACK);
  tft.setFont(&g_font_cjk);
  tft.setTextSize(1);
  tft.setTextWrap(false);

  // 轻量占位图，避免用户看到长时间纯黑屏
  tft.drawCircle(120, 88, 36, TFT_DARKGREY);
  tft.drawCircle(120, 88, 37, TFT_DARKGREY);
  tft.fillCircle(120, 88, 4, TFT_WHITE);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  draw_center_text("正在切换...", 142);

  if (title && title[0]) {
    draw_centered_wrapped_2lines(&tft, String(title), 120, 166, 16, 190, TFT_WHITE);
  }
  if (artist && artist[0]) {
    tft.setTextColor(0xAD55, TFT_BLACK);
    draw_center_text(artist, 206);
  }

  s_screen_cleared = true;
  ui_draw_unlock();
  ui_request_refresh();
}


void ui_return_to_player(void)
{
  ui_draw_lock();
  ui_set_screen(UI_SCREEN_PLAYER);
  tft.fillScreen(TFT_BLACK);
  // 已清屏，设置清屏标志
  s_screen_cleared = true;

  // 不要清 s_src / s_coverSprReady / now playing 数据
  // 只做"请求完整重绘"
  s_rot_last_ms = millis();
  ui_draw_unlock();
  ui_request_refresh();
}

void ui_enter_nfc_admin(void)
{
  ui_draw_lock();
  ui_set_screen(UI_SCREEN_NFC_ADMIN);
  tft.fillScreen(TFT_BLACK);
  // 离开播放器界面，复位清屏标志
  s_screen_cleared = false;
  ui_draw_unlock();
  ui_request_refresh();
}

static const char* target_type_to_cn(NfcAdminTargetType type)
{
  switch (type) {
    case NFC_ADMIN_TARGET_TRACK:  return "单曲";
    case NFC_ADMIN_TARGET_ARTIST: return "歌手";
    case NFC_ADMIN_TARGET_ALBUM:  return "专辑";
    default:                      return "未知";
  }
}

void ui_nfc_admin_show_wait_card(const NfcAdminTarget& target)
{
  ui_draw_lock();
  ui_set_screen(UI_SCREEN_NFC_ADMIN);

  tft.fillScreen(TFT_BLACK);
  // 离开播放器界面，复位清屏标志
  s_screen_cleared = false;
  tft.setTextDatum(middle_center);

  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  draw_center_text("请刷卡", 38);
  tft.setTextSize(1);

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  String type_str = String("绑定类型：") + target_type_to_cn(target.type);
  draw_center_text(type_str.c_str(), 92);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  int text_y = 118;
  int x0, w;
  circle_span(text_y, 12, x0, w); // 左右各留 12px
  int max_w = w;
  draw_centered_wrapped_2lines(&tft, 
                               target.display, 
                               120, 
                               text_y, 
                               22, 
                               max_w, 
                               TFT_WHITE);
  
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  draw_center_text("MODE：返回", 152);

  ui_draw_unlock();
  ui_request_refresh();
}

static const char* nfc_ui_target_type_to_cn(NfcUiTargetType type)
{
  switch (type) {
    case NFC_UI_TARGET_TRACK:  return "单曲";
    case NFC_UI_TARGET_ARTIST: return "歌手";
    case NFC_UI_TARGET_ALBUM: return "专辑";
    default:                  return "未知";
  }
}

void ui_nfc_admin_show_confirm(const String& uid, NfcUiConfirmState state, NfcUiTargetType old_type, const String& old_name, NfcUiTargetType new_type, const String& new_name)
{
  ui_draw_lock();
  tft.fillScreen(TFT_BLACK);
  // 离开播放器界面，复位清屏标志
  s_screen_cleared = false;
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  draw_center_text("卡片已识别", 40);
  
  tft.setTextSize(1);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  draw_center_text(uid.c_str(), 70);
  
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  char buf[64];
  snprintf(buf, sizeof(buf), "绑定到: %s", nfc_ui_target_type_to_cn(new_type));
  draw_center_text(buf, 90);
  
  int new_y = 110;
  int x0, w;
  circle_span(new_y, 14, x0, w); // 左右各留 14px
  int new_w = w;
  draw_centered_wrapped_2lines(&tft, 
                               new_name, 
                               120, 
                               new_y, 
                               20, 
                               new_w, 
                               TFT_CYAN);
  
  if (state == NFC_UI_CONFIRM_REPLACE) {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    draw_center_text("将替换现有绑定", 140);
    tft.setTextSize(1);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    int old_y = 160;
    circle_span(old_y, 14, x0, w); // 左右各留 14px
    int old_w = w;
    draw_centered_wrapped_2lines(&tft, 
                                 old_name, 
                                 120, 
                                 old_y, 
                                 20, 
                                 old_w, 
                                 TFT_YELLOW);
  } else if (state == NFC_UI_CONFIRM_SAME) {
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    draw_center_text("绑定相同，无需更改", 175);
  }
  
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  draw_center_text("PLAY：保存  MODE：返回", 200);
  ui_draw_unlock();
  ui_request_refresh();
}

void ui_nfc_admin_show_saving()
{
  ui_draw_lock();
  tft.fillScreen(TFT_BLACK);
  // 离开播放器界面，复位清屏标志
  s_screen_cleared = false;
  tft.setTextSize(2);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  draw_center_text("保存中...", 120);
  ui_draw_unlock();
  ui_request_refresh();
}

void ui_nfc_admin_show_wait_remove(const String& uid)
{
  (void)uid;
  ui_draw_lock();
  tft.fillScreen(TFT_BLACK);
  // 离开播放器界面，复位清屏标志
  s_screen_cleared = false;
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  draw_center_text("请移开卡片", 120);
  ui_draw_unlock();
  ui_request_refresh();
}

void ui_nfc_admin_show_error(const char* msg)
{
  ui_draw_lock();
  tft.fillScreen(TFT_BLACK);
  // 离开播放器界面，复位清屏标志
  s_screen_cleared = false;
  tft.setTextSize(2);
  tft.setTextColor(TFT_RED, TFT_BLACK);
  draw_center_text("错误", 80);
  
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  draw_center_text(msg, 120);
  
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  draw_center_text("MODE：返回", 150);
  ui_draw_unlock();
  ui_request_refresh();
}

void ui_nfc_admin_show_done()
{
  ui_draw_lock();
  tft.fillScreen(TFT_BLACK);
  // 离开播放器界面，复位清屏标志
  s_screen_cleared = false;
  tft.setTextSize(2);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  draw_center_text("完成", 100);
  
  tft.setTextSize(1);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  draw_center_text("即将返回播放器", 130);
  ui_draw_unlock();
  ui_request_refresh();
}

void ui_show_scanning()
{
  ui_scan_begin();
}

// =============================================================================
// 扫描 UI（由 storage_music.cpp / keys.cpp 使用）
// =============================================================================

void ui_scan_begin()
{
  ui_draw_lock();
  ui_set_screen(UI_SCREEN_BOOT);

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  tft.setTextSize(2);
  draw_center_text("正在扫描", 60);

  tft.setTextSize(1);
  draw_center_text("请稍候", 100);

  // 清除动画和计数区域
  tft.fillRect(0, 130, 240, 110, TFT_BLACK);

  s_scan_last_ms = 0;
  s_scan_phase = 0;
  // 离开播放器界面，复位清屏标志
  s_screen_cleared = false;
  ui_draw_unlock();
  ui_request_refresh();
}

// 绘制扫描动画的点
// 参数: phase - 当前动画阶段 (0, 1, 2)
// 功能: 绘制三个点，根据 phase 参数高亮显示其中一个点，形成动画效果
static void draw_scan_dots(int phase)
{
  // 计算点的中心位置和间距
  int cx = 120;
  int y  = 155;
  int dx = 18;

  // 清除点区域
  tft.fillRect(cx - 40, y - 12, 80, 24, TFT_BLACK);

  // 绘制三个点
  for (int i = 0; i < 3; i++) {
    int x = cx + (i - 1) * dx;
    // 当前阶段填充圆点，其他阶段绘制空心圆点
    if (i == phase) tft.fillCircle(x, y, 5, TFT_WHITE);
    else            tft.drawCircle(x, y, 5, TFT_WHITE);
  }
}

void ui_scan_tick(int tracks_count)
{
  uint32_t now = millis();
  if (now - s_scan_last_ms < 150) return;
  s_scan_last_ms = now;

  s_scan_phase = (s_scan_phase + 1) % 3;

  ui_draw_lock();
  draw_scan_dots(s_scan_phase);

  // 更新曲目计数
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.fillRect(0, 185, 240, 30, TFT_BLACK);
  draw_center_text(String("已扫描 " + String(tracks_count) + " 首歌曲").c_str(), 190);
  ui_draw_unlock();
}

void ui_scan_end()
{
  // 无操作
}

void ui_scan_abort()
{
  ui_draw_lock();

  // 显示"已取消"提示
  tft.fillScreen(TFT_BLACK);
  // 离开播放器界面，复位清屏标志
  s_screen_cleared = false;
  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  tft.setTextSize(2);
  draw_center_text("已取消", 100);

  tft.setTextSize(1);
  draw_center_text("扫描已中断", 140);

  ui_draw_unlock();
  // 延迟一段时间让用户看到提示，但不要长时间占着 UI/SPI 锁
  delay(1500);
}

void ui_clear_screen()
{
  ui_draw_lock();
  // 清除屏幕
  tft.fillScreen(TFT_BLACK);
  // 清屏后设置清屏标志
  s_screen_cleared = true;
  ui_draw_unlock();
  ui_request_refresh();
}
