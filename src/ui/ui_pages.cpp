#include <Arduino.h>
#include <cstring>
#include "ui/ui_internal.h"
#include "ui/ui_text_utils.h"
#include "utils/log.h"
#undef LOG_TAG
#define LOG_TAG "UI"

void ui_show_message(const char* msg)
{
  if (!msg) msg = "";
  LOGD("[界面] 消息: %s", msg);

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

  // 重置清屏标志，确保下次渲染时清屏
  s_screen_cleared = false;

  // 延迟清屏：保持启动界面直到音乐加载完成
  // tft.fillScreen(TFT_BLACK);

  s_angle_deg = 0.0f;
  s_rot_last_ms = millis();
  ui_unlock();
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

void ui_show_player_placeholder(const char* line1, const char* line2)
{
  if (!line1) line1 = "";
  if (!line2) line2 = "";

  ui_draw_lock();

  ui_set_screen(UI_SCREEN_PLAYER);

  // 关键：这是一个稳定占位页，不要再触发“5秒后加载中”兜底
  s_player_enter_time = 0;

  // 清掉旧封面状态，避免无卡时旧封面继续转
  s_coverSprReady = false;
  s_src = nullptr;
  s_angle_deg = 0.0f;
  s_rot_last_ms = millis();

  tft.fillScreen(TFT_BLACK);
  tft.setFont(&g_font_cjk);
  tft.setTextWrap(false);

  // 简单画一个唱片占位图
  tft.drawCircle(120, 82, 38, TFT_DARKGREY);
  tft.drawCircle(120, 82, 39, TFT_DARKGREY);
  tft.fillCircle(120, 82, 5, TFT_WHITE);

  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  draw_center_text(line1, 142);

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  draw_center_text(line2, 168);

  s_screen_cleared = true;

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

    // 显示旧绑定类型 + 旧绑定名称，格式：单曲-XXX / 歌手-XXX / 专辑-XXX。
    // 保持原来的确认页逻辑，只补充专辑/单曲/歌手类型说明。
    tft.setTextSize(1);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    String old_desc = String(nfc_ui_target_type_to_cn(old_type)) + "-" + old_name;
    int old_y = 160;
    circle_span(old_y, 14, x0, w); // 左右各留 14px
    int old_w = w;
    draw_centered_wrapped_2lines(&tft,
                                 old_desc,
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
  draw_center_text("PLAY键 / 旋钮按下：保存", 200);
  draw_center_text("MODE键：返回", 216);
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
  ui_scan_begin(false, false, false, false);
}

// =============================================================================
// 扫描 UI
// =============================================================================

static const char* scan_mode_label(bool full_scan,
                                   bool forced_full_scan,
                                   bool strict_incremental,
                                   bool ultra_fast_incremental)
{
  if (!full_scan) {
    if (ultra_fast_incremental) return "超快速目录扫描";
    return strict_incremental ? "严格增量扫描" : "快速增量扫描";
  }
  return forced_full_scan ? "强制全量扫描" : "自动全量扫描";
}

static void scan_current_name(const char* current_path,
                              char* out,
                              size_t out_size)
{
  if (!out || out_size == 0) return;
  out[0] = '\0';

  if (!current_path || !current_path[0]) {
    snprintf(out, out_size, "扫描目录中");
    return;
  }

  const char* name = strrchr(current_path, '/');
  name = name ? name + 1 : current_path;
  const size_t length = strlen(name);

  if (length < out_size) {
    snprintf(out, out_size, "%s", name);
    return;
  }

  const size_t keep = out_size > 5 ? out_size - 5 : 0;
  if (keep == 0) {
    snprintf(out, out_size, "%s", name);
    return;
  }
  snprintf(out, out_size, "...%s", name + length - keep);
}

void ui_scan_begin(bool full_scan,
                   bool forced_full_scan,
                   bool strict_incremental,
                   bool ultra_fast_incremental)
{
  ui_draw_lock();
  ui_set_screen(UI_SCREEN_BOOT);

  tft.fillScreen(TFT_BLACK);
  tft.setFont(&g_font_cjk);
  tft.setTextWrap(false);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  tft.setTextSize(2);
  draw_center_text("曲库重扫", 20);

  tft.setTextSize(1);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  draw_center_text(scan_mode_label(full_scan, forced_full_scan, strict_incremental, ultra_fast_incremental), 50);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  draw_center_text("正在准备...", 98);
  draw_center_text("按 MODE 键可取消", 222);

  s_scan_last_ms = 0;
  s_scan_phase = 0;
  s_screen_cleared = false;
  ui_draw_unlock();
  ui_request_refresh();
}

static void draw_scan_dots(int phase)
{
  const int cx = 120;
  const int y = 151;
  const int dx = 18;

  tft.fillRect(cx - 40, y - 12, 80, 24, TFT_BLACK);
  for (int i = 0; i < 3; ++i) {
    const int x = cx + (i - 1) * dx;
    if (i == phase) tft.fillCircle(x, y, 5, TFT_WHITE);
    else tft.drawCircle(x, y, 5, TFT_WHITE);
  }
}

void ui_scan_tick(const UiScanProgress& progress)
{
  const uint32_t now = millis();
  if (now - s_scan_last_ms < 150) return;
  s_scan_last_ms = now;
  s_scan_phase = (s_scan_phase + 1) % 3;

  char line[64];
  char current[34];
  scan_current_name(progress.current_path, current, sizeof(current));

  ui_draw_lock();
  tft.setFont(&g_font_cjk);
  tft.setTextWrap(false);
  tft.setTextSize(1);

  tft.fillRect(0, 39, 240, 177, TFT_BLACK);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  draw_center_text(
      scan_mode_label(progress.full_scan,
                      progress.forced_full_scan,
                      progress.strict_incremental,
                      progress.ultra_fast_incremental),
      50);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  snprintf(line, sizeof(line), "发现 %lu  复用 %lu",
           (unsigned long)progress.discovered,
           (unsigned long)progress.reused);
  draw_center_text(line, 78);

  snprintf(line, sizeof(line), "新增 %lu  修改 %lu",
           (unsigned long)progress.added,
           (unsigned long)progress.modified);
  draw_center_text(line, 100);

  if (progress.full_scan) {
    snprintf(line, sizeof(line), "全量解析进行中");
  } else if (progress.ultra_fast_incremental) {
    snprintf(line, sizeof(line), "已跳过目录 %lu",
             (unsigned long)progress.directories_skipped);
  } else {
    snprintf(line, sizeof(line), "删除将在完成后统计");
  }
  draw_center_text(line, 122);

  draw_scan_dots(s_scan_phase);

  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  draw_center_text("当前文件", 178);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  draw_center_text(current, 198);

  ui_draw_unlock();
}

void ui_scan_tick(int tracks_count)
{
  UiScanProgress progress{};
  progress.full_scan = true;
  progress.forced_full_scan = true;
  progress.strict_incremental = false;
  progress.ultra_fast_incremental = false;
  progress.discovered = tracks_count > 0 ? (uint32_t)tracks_count : 0;
  ui_scan_tick(progress);
}

void ui_scan_end()
{
  // 扫描完成后仍需构建和保存索引，最终结果由 ui_scan_complete() 显示。
}

void ui_scan_complete(const UiScanSummary& summary)
{
  char line[64];

  ui_draw_lock();
  tft.fillScreen(TFT_BLACK);
  tft.setFont(&g_font_cjk);
  tft.setTextWrap(false);

  tft.setTextSize(2);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  draw_center_text("扫描完成", 24);

  tft.setTextSize(1);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  draw_center_text(
      scan_mode_label(summary.full_scan,
                      summary.forced_full_scan,
                      summary.strict_incremental,
                      summary.ultra_fast_incremental),
      55);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  snprintf(line, sizeof(line), "发现 %lu  复用 %lu",
           (unsigned long)summary.discovered,
           (unsigned long)summary.reused);
  draw_center_text(line, 88);

  snprintf(line, sizeof(line), "新增 %lu  修改 %lu",
           (unsigned long)summary.added,
           (unsigned long)summary.modified);
  draw_center_text(line, 112);

  snprintf(line, sizeof(line), "删除 %lu  用时 %lu.%lu秒",
           (unsigned long)summary.deleted,
           (unsigned long)(summary.elapsed_ms / 1000u),
           (unsigned long)((summary.elapsed_ms % 1000u) / 100u));
  draw_center_text(line, 136);

  if (summary.unchanged) {
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    draw_center_text("无变化，已跳过索引重建", 162);
  } else if (summary.ultra_fast_incremental) {
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    snprintf(line, sizeof(line), "跳过目录 %lu",
             (unsigned long)summary.directories_skipped);
    draw_center_text(line, 162);
  }

  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  draw_center_text("即将返回播放器", 196);
  s_screen_cleared = false;
  ui_draw_unlock();

  delay(1600);
}

void ui_scan_failed(bool full_scan)
{
  ui_draw_lock();
  tft.fillScreen(TFT_BLACK);
  tft.setFont(&g_font_cjk);
  tft.setTextWrap(false);
  tft.setTextSize(2);
  tft.setTextColor(TFT_RED, TFT_BLACK);
  draw_center_text("扫描失败", 78);

  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  draw_center_text(full_scan ? "强制全量扫描未完成" : "曲库重扫未完成", 122);
  draw_center_text("请查看串口错误日志", 150);
  s_screen_cleared = false;
  ui_draw_unlock();

  delay(1500);
}

void ui_scan_abort()
{
  ui_draw_lock();
  tft.fillScreen(TFT_BLACK);
  tft.setFont(&g_font_cjk);
  tft.setTextWrap(false);
  s_screen_cleared = false;
  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  tft.setTextSize(2);
  draw_center_text("已取消", 100);

  tft.setTextSize(1);
  draw_center_text("扫描已中断", 140);

  ui_draw_unlock();
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
