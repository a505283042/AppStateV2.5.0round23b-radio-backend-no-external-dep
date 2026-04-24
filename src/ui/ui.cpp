#include <Arduino.h>
#include "board/board_spi.h"
#include "ui/ui_internal.h"
#include "ui/ui_text_utils.h"
#include "utils/log.h"
#undef LOG_TAG
#define LOG_TAG "UI"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "fonts/u8g2_font_wenquanyi_merged.h"
#include "lyrics/lyrics.h"
#include "player_list_select.h"
#include "audio/audio.h"
#include "audio/audio_service.h"

lgfx::U8g2font g_font_cjk(u8g2_font_wenquanyi_merged);

static int utf8_char_len(uint8_t c)
{
  if ((c & 0x80) == 0) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1;
}

TaskHandle_t s_ui_task = nullptr;
SemaphoreHandle_t s_ui_mtx = nullptr;
bool s_rotate_wait_prefetch_done = false;

ui_screen_t s_screen = UI_SCREEN_BOOT;
LGFX tft;
bool s_screen_cleared = false;
uint32_t s_player_enter_time = 0;

volatile enum ui_player_view_t s_view = UI_VIEW_INFO;
String s_np_title;
String s_np_artist;
String s_np_album;
int s_title_scroll_x = 0;
int s_artist_scroll_x = 0;
int s_album_scroll_x = 0;
uint32_t s_scroll_last_ms = 0;

volatile uint8_t s_ui_volume = 100;
volatile play_mode_t s_ui_play_mode = PLAY_MODE_ALL_SEQ;
volatile int s_ui_track_idx = 0;
volatile int s_ui_track_total = 0;
volatile uint32_t s_ui_volume_active_time = UINT32_MAX;
volatile uint32_t s_ui_mode_switch_time = 0;
volatile uint32_t s_ui_play_ms = 0;
volatile uint32_t s_ui_total_ms = 0;

LGFX_Sprite s_coverSpr(&tft);
LGFX_Sprite s_coverMasked(&tft);
bool s_coverSprInited = false;
bool s_coverSprReady = false;
LGFX_Sprite s_coverCacheSpr0(&tft);
LGFX_Sprite s_coverCacheMasked0(&tft);
LGFX_Sprite s_coverCacheSpr1(&tft);
LGFX_Sprite s_coverCacheMasked1(&tft);
LGFX_Sprite* s_coverCacheSpr[2] = { &s_coverCacheSpr0, &s_coverCacheSpr1 };
LGFX_Sprite* s_coverCacheMasked[2] = { &s_coverCacheMasked0, &s_coverCacheMasked1 };
bool s_coverCacheInited = false;
bool s_coverCacheReady[2] = { false, false };
int s_coverCacheTrackIdx[2] = { -1, -1 };

LGFX_Sprite s_frame0(&tft);
LGFX_Sprite s_frame1(&tft);
LGFX_Sprite* s_frame[2] = { &s_frame0, &s_frame1 };
uint8_t s_front = 0;
uint8_t s_back = 1;
bool s_framesInited = false;

LGFX_Sprite s_rotFrame0(&tft);
LGFX_Sprite s_rotFrame1(&tft);
LGFX_Sprite* s_rotFrame[2] = { &s_rotFrame0, &s_rotFrame1 };
uint8_t s_rotFront = 0;
uint8_t s_rotBack = 1;
bool s_rotFramesInited = false;
LGFX_Sprite* s_src = nullptr;

int s_list_last_drawn_idx = -1;
float s_angle_deg = 0.0f;
uint32_t s_rot_last_ms = 0;
bool s_rotate_wait_audio_start = false;
uint32_t s_cover_apply_ms = 0;
uint32_t s_rotate_release_ms = 0;
uint32_t s_rotate_release_audio_ms = 0;
int s_rotate_probe_frames_left = 0;

uint32_t s_scan_last_ms = 0;
int s_scan_phase = 0;
volatile bool s_ui_hold = false;

void ui_lock()
{
  if (s_ui_mtx) xSemaphoreTakeRecursive(s_ui_mtx, portMAX_DELAY);
}

void ui_unlock()
{
  if (s_ui_mtx) xSemaphoreGiveRecursive(s_ui_mtx);
}

void ui_draw_lock()
{
  ui_lock();
  board_spi_ui_lock();
}

void ui_draw_unlock()
{
  board_spi_ui_unlock();
  ui_unlock();
}

void ui_request_refresh()
{
  if (s_ui_task) {
    xTaskNotifyGive(s_ui_task);
  }
}

void ui_request_refresh_now()
{
  ui_request_refresh();
}

void ui_set_rotate_wait_prefetch(bool wait)
{
  s_rotate_wait_prefetch_done = wait;
  if (!wait) {
    ui_request_refresh();
  }
}

void ui_hold_render(bool hold)
{
  s_ui_hold = hold;
  if (hold) {
    s_rot_last_ms = millis();
  } else {
    s_rot_last_ms = millis();
    if (s_ui_task) {
      xTaskNotifyGive(s_ui_task);
    }
  }
}
static inline TickType_t ui_period_ticks()
{
  // hold 期间：不画，但要"醒得勤快一点"，保证解除 hold 后立刻恢复（这里按旋转帧率）
  if (s_ui_hold) return pdMS_TO_TICKS(1000 / UI_FPS_ROTATE);

  // 列表选择模式：使用较高帧率以实现平滑滚动
  if (player_list_select_is_active()) return pdMS_TO_TICKS(1000 / 20);

  // PLAYER 界面：按视图区分帧率
  if (s_screen == UI_SCREEN_PLAYER) {
    if (s_view == UI_VIEW_ROTATE) return pdMS_TO_TICKS(1000 / UI_FPS_ROTATE);

    const bool info_active = audio_service_is_playing() && !audio_service_is_paused();
    const uint32_t fps = info_active ? UI_FPS_INFO_ACTIVE : UI_FPS_INFO_IDLE;
    return pdMS_TO_TICKS(1000 / fps);
  }

  // 其它界面：1fps
  return pdMS_TO_TICKS(1000 / UI_FPS_OTHER);
}

static void ui_task_entry(void*)
{
  for (;;) {
    // 动态帧率：rotate 20fps / info 自适应 / other 1fps
    TickType_t period = ui_period_ticks();
    if (period == 0) period = 1;

    // 使用 ulTaskNotifyTake 实现可中断的延迟
    // 正常情况下等待 period 时长，但收到通知时立即唤醒
    ulTaskNotifyTake(pdTRUE, period);
    const uint32_t now_ms = millis();

    // hold：不渲染，但刷新 rot 时钟，避免解除 hold 后 dt 巨大导致角度跳变
    if (s_ui_hold) {
      s_rot_last_ms = millis();
      continue;
    }

    // 检查是否处于列表选择模式
    if (player_list_select_is_active()) {
      ui_draw_lock();

      int current_idx = player_list_select_get_selected_idx();
      ListSelectState state = player_list_select_get_state();

      if (state == ListSelectState::TRACKS) {
        const auto& tracks = player_list_select_get_tracks();
        ui_draw_track_select(tracks, current_idx, "选择歌曲");
      } else {
        const char* title = (state == ListSelectState::ARTIST) ? "选择歌手" : "选择专辑";
        const auto& groups = player_list_select_get_groups();
        ui_draw_list_select(groups, current_idx, title);
      }

      s_list_last_drawn_idx = current_idx;
      ui_draw_unlock();
      continue;
    }

    // 只在 PLAYER 界面、封面就绪时推屏
    if (s_screen == UI_SCREEN_PLAYER && s_coverSprReady && s_framesInited) {
      ui_draw_lock();

      // 第一次渲染时清屏，避免启动界面残留
      if (!s_screen_cleared) {
        tft.fillScreen(TFT_BLACK);
        s_screen_cleared = true;
      }

      // 更新歌词时间（在绘制前更新）
      uint32_t play_ms = audio_get_play_ms();
      g_lyricsDisplay.updateTime(play_ms);

      if (s_view == UI_VIEW_ROTATE && s_src) {
        if (s_rot_last_ms == 0) s_rot_last_ms = now_ms;

        if (s_rotate_wait_audio_start || s_rotate_wait_prefetch_done) {
          const uint32_t audio_ms_now = audio_get_play_ms();
          if (s_rotate_wait_audio_start && audio_service_is_playing() && audio_ms_now > 0) {
            s_rotate_wait_audio_start = false;
          }
          if (!(s_rotate_wait_audio_start || s_rotate_wait_prefetch_done)) {
            s_rot_last_ms = now_ms;
            s_rotate_release_ms = now_ms;
            s_rotate_release_audio_ms = audio_ms_now;
            s_rotate_probe_frames_left = 6;
            LOGI("[UI] rotate release audio_ms=%lu cover_age=%lums", (unsigned long)audio_ms_now, (unsigned long)(now_ms - s_cover_apply_ms));
          } else {
            s_rot_last_ms = now_ms;
            s_coverSpr.pushSprite(0, 0);
            ui_draw_unlock();
            continue;
          }
        }

        float dt = (now_ms - s_rot_last_ms) * 0.001f;
        s_rot_last_ms = now_ms;

        // 防止任何阻塞导致 dt 过大（看起来像“后台一直在转”）
        if (dt > 0.20f) dt = 0.20f;

        // 暂停时不旋转封面
        if (!audio_service_is_paused()) {
          s_angle_deg += COVER_DEG_PER_SEC * dt;
          if (s_angle_deg >= 360.0f) s_angle_deg -= 360.0f;
        }

        const uint32_t rotate_frame_begin = millis();
        cover_rotate_draw(s_angle_deg);
        const uint32_t rotate_frame_end = millis();
        if (s_rotate_probe_frames_left > 0) {
          const uint32_t audio_ms_now = audio_get_play_ms();
          const int frame_idx = 7 - s_rotate_probe_frames_left;
          LOGI("[UI] rotate probe frame=%d audio_ms=%lu audio_since_release=%lums since_release=%lums draw=%lums total=%lums", frame_idx, (unsigned long)audio_ms_now, (unsigned long)(audio_ms_now > s_rotate_release_audio_ms ? (audio_ms_now - s_rotate_release_audio_ms) : 0), (unsigned long)(rotate_frame_begin - s_rotate_release_ms), (unsigned long)(rotate_frame_end - rotate_frame_begin), (unsigned long)(rotate_frame_end - rotate_frame_begin));
          --s_rotate_probe_frames_left;
        }
      } else {
        // INFO：显示正向封面+信息，同时刷新 rot 时钟，回到旋转不跳角度
        s_rot_last_ms = now_ms;
        cover_info_draw();
      }

      ui_draw_unlock();
    } else if (s_screen == UI_SCREEN_PLAYER) {
      // 兜底：如果进入播放器界面超过5秒还没就绪，显示轻量占位页
      if (s_player_enter_time > 0 && (now_ms - s_player_enter_time) > 5000 && !s_screen_cleared) {
        ui_draw_lock();
        tft.fillScreen(TFT_BLACK);
        tft.setFont(&g_font_cjk);
        tft.setTextSize(1);
        tft.setTextWrap(false);
        
        // 轻量占位图
        tft.drawCircle(120, 88, 36, TFT_DARKGREY);
        tft.drawCircle(120, 88, 37, TFT_DARKGREY);
        tft.fillCircle(120, 88, 4, TFT_WHITE);

        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        draw_center_text("加载中...", 142);
        draw_center_text("请稍候", 166);
        
        s_screen_cleared = true;
        ui_draw_unlock();
      }
      // 非 PLAYER：也刷新 rot 时钟，避免回到旋转 dt 累积
      s_rot_last_ms = now_ms;
    } else {
      // 非 PLAYER：也刷新 rot 时钟，避免回到旋转 dt 累积
      s_rot_last_ms = now_ms;
    }
  }
}

static constexpr uint32_t kUiTaskStackBytes = 4096; // UI 任务栈大小

static void ui_task_start_once()
{

  if (s_ui_task) return;

  if (!s_ui_mtx) s_ui_mtx = xSemaphoreCreateRecursiveMutex();

  // UiTask 固定 core1，低优先级（比音频低很多）
  xTaskCreatePinnedToCore(
    ui_task_entry,
    "UiTask",
    kUiTaskStackBytes,
    nullptr,
    1,      // 低优先级
    &s_ui_task,
    1       // core1
  );
}

void ui_init(void)
{
  LOGI("[UI] init (LGFX GC9A01)");

  ui_draw_lock();
  tft.init();
  tft.setRotation(3); // 旋转 270 度

  tft.initDMA();
  LOGI("[UI] DMA initialized");

  cover_sprite_init_once();
  cover_cache_sprite_init_once();
  cover_frames_init_once();

  ui_task_start_once();
  
  // 确保 UI 任务创建完成后再开启渲染开关
  s_ui_hold = false;

  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  draw_center_text("ESP32 Player", 90);
  tft.setTextSize(1);
  draw_center_text("启动中...", 130);

  s_screen = UI_SCREEN_BOOT;
  ui_draw_unlock();
}

void ui_set_screen(ui_screen_t screen)
{
  s_screen = screen;
  LOGI("[UI] switch screen -> %d", (int)screen);
}

TaskHandle_t ui_get_task_handle(void)
{
  return s_ui_task;
}
