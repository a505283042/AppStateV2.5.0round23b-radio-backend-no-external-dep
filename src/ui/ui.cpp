#include <Arduino.h>
#include "board/board_spi.h"
#include "ui/ui_internal.h"
#include "ui/ui_text_utils.h"
#include "ui/ui_list_select_view.h"
#include "ui/ui_quick_menu_view.h"
#include "menu/quick_menu.h"
#include "utils/log.h"
#include "app_diagnostics.h"
#include "web/web_settings.h"
#undef LOG_TAG
#define LOG_TAG "UI"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "fonts/u8g2_font_wenquanyi_merged.h"
#include "lyrics/lyrics.h"
#include "player_list_select.h"
#include "audio/audio.h"
#include "audio/audio_service.h"
#include "hal/board_hw_control.h"

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
bool s_quick_menu_was_active = false;
bool s_list_select_was_active = false;

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
static constexpr uint32_t kUiDiagQuickMenuDrawMs = 60;
static constexpr uint32_t kUiDiagQuickMenuLogIntervalMs = 5000;
static uint32_t s_ui_diag_last_quick_menu_log_ms = 0;

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

static bool ui_player_audio_active_for_cover_spin()
{
  // 未开播时 audio_service_is_paused() 通常为 false，不能只用“非暂停”判断。
  // 只有音频服务明确处于播放状态时，旋转封面/面板视图才允许转动。
  return audio_service_is_playing() && !audio_service_is_paused();
}

static bool ui_player_has_track_for_cover_angle()
{
  // 已暂停时保留当前角度，只是不继续转；完全未开播时才归零显示。
  return audio_service_is_playing() || audio_service_is_paused();
}

static float ui_cover_draw_angle_or_zero()
{
  return (web_settings_get().web_cover_spin && ui_player_has_track_for_cover_angle())
      ? s_angle_deg
      : 0.0f;
}

static inline TickType_t ui_period_ticks()
{
  // hold 期间：不画，但要"醒得勤快一点"，保证解除 hold 后立刻恢复（这里按旋转帧率）
  if (s_ui_hold) return pdMS_TO_TICKS(1000 / UI_FPS_ROTATE);


  // 列表选择模式优先于快捷菜单。
  // 从快捷菜单进入列表时菜单仍保持 active，帧率判断也要优先按列表处理，
  // 避免列表刷新被菜单状态降级或延后。
  if (player_list_select_is_active()) return pdMS_TO_TICKS(1000 / UI_FPS_LIST_SELECT);

  // 快捷菜单：不需要高帧率，但需要比 1fps 更跟手。
  if (quick_menu_is_active()) return pdMS_TO_TICKS(1000 / UI_FPS_QUICK_MENU);

  // PLAYER 界面：按视图区分帧率。
  // 封面旋转关闭后降低刷新压力，但面板视图仍保留歌词/进度刷新。
  if (s_screen == UI_SCREEN_PLAYER) {
    const bool cover_spin_enabled = web_settings_get().web_cover_spin;
    const bool player_active = ui_player_audio_active_for_cover_spin();
    const bool cover_should_spin = cover_spin_enabled && player_active;

    if (s_view == UI_VIEW_ROTATE) {
      const uint32_t fps = cover_should_spin
          ? UI_FPS_ROTATE
          : UI_FPS_ROTATE_STATIC;
      return pdMS_TO_TICKS(1000 / fps);
    }

    if (s_view == UI_VIEW_COVER_PANEL) {
      const uint32_t fps = cover_should_spin
          ? UI_FPS_COVER_PANEL
          : (player_active ? UI_FPS_COVER_PANEL_STATIC_ACTIVE : UI_FPS_COVER_PANEL_STATIC_IDLE);
      return pdMS_TO_TICKS(1000 / fps);
    }

    const uint32_t fps = player_active ? UI_FPS_INFO_ACTIVE : UI_FPS_INFO_IDLE;
    return pdMS_TO_TICKS(1000 / fps);
  }

  // 其它界面：1fps
  return pdMS_TO_TICKS(1000 / UI_FPS_OTHER);
}

static void ui_task_entry(void*)
{
  for (;;) {
    // 电池状态后台采样（内部 1 分钟采样一次）
    board_hw_battery_status_tick();

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

    // 菜单计时由 loopTask/keys_update() 推进；UI 任务只负责绘制。
    // 播放源菜单打开列表时会保留 quick_menu active，方便 MODE 短按返回菜单；
    // 如果这里先画菜单，屏幕会停在菜单页，出现“列表能选能播但 UI 不变”。
    
    if (player_list_select_is_active()) {
      s_list_select_was_active = true;
      // 菜单/列表覆盖播放器时，暂停封面旋转时钟，避免退出后 dt 累积导致角度跳变。
      s_rot_last_ms = now_ms;

      ui_draw_lock();

      int current_idx = player_list_select_get_selected_idx();
      ListSelectState state = player_list_select_get_state();

      if (state == ListSelectState::RADIO) {
        const auto& radios = player_list_select_get_radios();
        ui_draw_radio_select(radios, current_idx, "选择电台");
      } else if (state == ListSelectState::NET_TRACK) {
        const auto& items = player_list_select_get_net_tracks();
        ui_draw_net_music_select(items,
                                player_list_select_get_net_track_page_start(),
                                current_idx,
                                player_list_select_get_net_track_total(),
                                "选择NAS歌曲");
      } else if (state == ListSelectState::TRACKS) {
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

    if (s_list_select_was_active) {
      s_list_select_was_active = false;

      if (quick_menu_is_active()) {
        // 列表刚退回菜单时，菜单内容本身可能没有 revision 变化，
        // 但屏幕已经被列表页覆盖，必须强制整屏重画一次。
        ui_quick_menu_view_reset();
        s_quick_menu_was_active = false;
      } else {
        // 列表直接退出到播放器时，也让播放器下一帧重新清一次背景。
        s_screen_cleared = false;
      }
    }

    if (quick_menu_is_active()) {
      if (!s_quick_menu_was_active) {
        ui_quick_menu_view_reset();
        s_quick_menu_was_active = true;
      }

      // 菜单覆盖播放器期间暂停封面旋转时钟；菜单空闲且内容未变化时不进绘图锁，
      // 避免 NAS 播放时持续抢占 SPI/CPU。按键会主动唤醒 UI，因此空闲低频不会影响跟手性。
      s_rot_last_ms = now_ms;
      if (ui_quick_menu_view_needs_draw()) {
        const uint32_t draw_t0 = millis();
        ui_draw_lock();
        ui_draw_quick_menu();
        ui_draw_unlock();
        const uint32_t draw_ms = millis() - draw_t0;
#if APP_DIAG_UI_RUNTIME
        AudioNetworkStateSnapshot network{};
        (void)audio_service_get_network_state(&network);
        if (network.active && draw_ms >= kUiDiagQuickMenuDrawMs) {
          const uint32_t log_now = millis();
          if (s_ui_diag_last_quick_menu_log_ms == 0 ||
              log_now - s_ui_diag_last_quick_menu_log_ms >= kUiDiagQuickMenuLogIntervalMs) {
            s_ui_diag_last_quick_menu_log_ms = log_now;
            LOGI("[界面诊断] 快捷菜单绘制耗时=%lums", (unsigned long)draw_ms);
          }
        }
#endif
      }
      continue;
    }

    // 刚退出菜单时，强制播放器页面重新清屏，避免菜单残影。
    if (s_quick_menu_was_active) {
      s_quick_menu_was_active = false;
      ui_quick_menu_view_reset();
      s_screen_cleared = false;
    }

    // 没有菜单/列表覆盖时，正常绘制播放器页面。
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

      if ((s_view == UI_VIEW_ROTATE || s_view == UI_VIEW_COVER_PANEL) && s_src) {
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
            LOGD("[界面] 旋转 release audio_ms=%lu 封面_age=%lums", (unsigned long)audio_ms_now, (unsigned long)(now_ms - s_cover_apply_ms));
          } else {
            s_rot_last_ms = now_ms;
            const float draw_angle_deg = ui_cover_draw_angle_or_zero();
            if (s_view == UI_VIEW_COVER_PANEL) {
              cover_panel_draw(draw_angle_deg);
            } else {
              // 旋转视图等待音频/封面预取期间也必须走完整绘制路径，
              // 不能直接 push 原封面，否则会把 NFC 弹窗覆盖掉，表现为“弹窗-封面-弹窗”闪烁。
              cover_rotate_draw(draw_angle_deg);
            }
            ui_draw_unlock();
            continue;
        }
        }

        float dt = (now_ms - s_rot_last_ms) * 0.001f;
        s_rot_last_ms = now_ms;

        // 防止任何阻塞导致 dt 过大（看起来像“后台一直在转”）
        if (dt > 0.20f) dt = 0.20f;

        // 只有真正开播且未暂停时才旋转封面；未开播时保持 0 度静态显示。
        const bool cover_should_spin = web_settings_get().web_cover_spin && ui_player_audio_active_for_cover_spin();
        if (cover_should_spin) {
          s_angle_deg += COVER_DEG_PER_SEC * dt;
          if (s_angle_deg >= 360.0f) s_angle_deg -= 360.0f;
        }

        // 未开播/关闭旋转时使用 0 度静态封面；暂停时保留当前角度但不继续转。
        const float draw_angle_deg = ui_cover_draw_angle_or_zero();

        const uint32_t rotate_frame_begin = millis();
        if (s_view == UI_VIEW_COVER_PANEL) {
          cover_panel_draw(draw_angle_deg);
        } else {
          cover_rotate_draw(draw_angle_deg);
        }
        const uint32_t rotate_frame_end = millis();
        if (s_rotate_probe_frames_left > 0) {
          const uint32_t audio_ms_now = audio_get_play_ms();
          const int frame_idx = 7 - s_rotate_probe_frames_left;
          LOGD("[界面] 旋转 探测 帧=%d audio_ms=%lu audio_since_release=%lums since_release=%lums draw=%lums 总计=%lums", frame_idx, (unsigned long)audio_ms_now, (unsigned long)(audio_ms_now > s_rotate_release_audio_ms ? (audio_ms_now - s_rotate_release_audio_ms) : 0), (unsigned long)(rotate_frame_begin - s_rotate_release_ms), (unsigned long)(rotate_frame_end - rotate_frame_begin), (unsigned long)(rotate_frame_end - rotate_frame_begin));
          --s_rotate_probe_frames_left;
        }
      } else {
        // INFO：显示正向封面+信息，同时刷新 rot 时钟，回到旋转不跳角度
        s_rot_last_ms = now_ms;
        cover_info_draw();
      }

      ui_draw_unlock();
      } else if (s_screen == UI_SCREEN_PLAYER) {
      // 播放器页但封面/帧缓冲还没 ready 时，会停留在启动或占位画面。
      // NFC 弹窗必须在这种“未开播/无封面”状态下也能显示，所以这里额外处理一次。
      const bool nfc_bind_popup_visible = ui_nfc_bind_target_popup_is_visible();
      const bool nfc_scan_popup_visible = ui_nfc_scan_popup_is_visible();
      const bool nfc_bind_popup_dirty = ui_nfc_bind_target_popup_consume_dirty();
      const bool nfc_scan_popup_dirty = ui_nfc_scan_popup_consume_dirty();
      const bool nfc_popup_visible = nfc_bind_popup_visible || nfc_scan_popup_visible;
      const bool nfc_popup_dirty = nfc_bind_popup_dirty || nfc_scan_popup_dirty;
      const bool placeholder_due =
          (s_player_enter_time > 0 && (now_ms - s_player_enter_time) > 5000 && !s_screen_cleared);

      if (placeholder_due || nfc_popup_visible || nfc_popup_dirty) {
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

        if (audio_service_is_playing() || audio_service_is_paused()) {
          draw_center_text("正在播放", 142);
          draw_center_text("封面加载中", 166);
        } else {
          draw_center_text("加载中...", 142);
          draw_center_text("请稍候", 166);
        }

        // 未开播/无封面时，NFC 弹窗直接画到 TFT 上，而不是等封面精灵路径。
        ui_draw_nfc_bind_target_popup_on_tft_if_visible();
        ui_draw_nfc_scan_popup_on_tft_if_visible();
        
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
  LOGI("[界面] 初始化屏幕（LGFX GC9A01）");

  ui_draw_lock();
  tft.init();
  tft.setRotation(3); // 旋转 270 度

  tft.initDMA();
  LOGI("[界面] DMA 初始化完成");

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
  LOGD("[界面] 切换屏幕 -> %d", (int)screen);
}

TaskHandle_t ui_get_task_handle(void)
{
  return s_ui_task;
}
