#pragma once

#include <Arduino.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <lgfx/v1/lgfx_fonts.hpp>

#include "ui/ui.h"
#include "ui/gc9a01_lgfx.h"

/**
 * @brief UI 内部共享状态头。
 *
 * 仅供 src/ui/*.cpp 之间共享，不建议业务层 include。
 * 这里放的是跨文件共享的：
 * - UiTask / 锁
 * - 播放页运行状态
 * - 封面精灵 / 双槽缓存
 * - 列表选择页绘图状态
 * - 电池状态缓存状态   
 */

static constexpr int COVER_SIZE = 240;                      // 封面大小
static constexpr float COVER_DEG_PER_SEC = 15.0f;           // 封面旋转角度 FPS
static constexpr uint32_t UI_FPS_ROTATE = 15;               // 封面旋转 FPS
static constexpr uint32_t UI_FPS_COVER_PANEL = 13;          // 封面面板滚动 FPS
static constexpr uint32_t UI_FPS_INFO_ACTIVE = 12;          // 信息面板滚动 FPS
static constexpr uint32_t UI_FPS_QUICK_MENU = 1;             // 快捷菜单空闲刷新 FPS：按键会主动唤醒，空闲时降低 UI 抢占
static constexpr uint32_t UI_FPS_LIST_SELECT = 10;           // 列表选择页刷新 FPS：按键会主动唤醒，避免 NAS 播放时持续高频重绘
static constexpr uint32_t UI_FPS_INFO_IDLE = 5;             // 信息面板空闲滚动 FPS 
static constexpr uint32_t UI_FPS_OTHER = 1;                 // 其他元素滚动 FPS

// 封面旋转关闭后的降帧策略。
// 旋转视图关闭旋转后只有静态封面，低帧率即可。
// 面板视图还有歌词、时间和进度环，播放中保留 5fps，避免进度条跳动明显。
static constexpr uint32_t UI_FPS_ROTATE_STATIC = 2;
static constexpr uint32_t UI_FPS_COVER_PANEL_STATIC_ACTIVE = 5;
static constexpr uint32_t UI_FPS_COVER_PANEL_STATIC_IDLE = 2;
static constexpr int SCROLL_SPEED = 1;                      // 滚动 FPS
static constexpr int SCROLL_GAP = 20;                       // 滚动间隔
static constexpr uint32_t VOLUME_ACTIVE_TIMEOUT_MS = 200;   // 音量激活超时时间
static constexpr uint32_t MODE_SWITCH_HIGHLIGHT_MS = 2000;  // 模式切换高亮时间

extern lgfx::U8g2font g_font_cjk;
extern TaskHandle_t s_ui_task;
extern SemaphoreHandle_t s_ui_mtx;
extern bool s_rotate_wait_prefetch_done;

extern ui_screen_t s_screen;
extern LGFX tft;
extern bool s_screen_cleared;
extern uint32_t s_player_enter_time;

extern volatile enum ui_player_view_t s_view;
extern String s_np_title;
extern String s_np_artist;
extern String s_np_album;
extern int s_title_scroll_x;
extern int s_artist_scroll_x;
extern int s_album_scroll_x;
extern uint32_t s_scroll_last_ms;

extern volatile uint8_t s_ui_volume;
extern volatile bool s_ui_volume_known;
extern volatile play_mode_t s_ui_play_mode;
extern volatile int s_ui_track_idx;
extern volatile int s_ui_track_total;
extern volatile uint32_t s_ui_volume_active_time;
extern volatile uint32_t s_ui_mode_switch_time;

/* 当前封面 + 双槽下一首封面缓存。 */
extern LGFX_Sprite s_coverSpr;
extern LGFX_Sprite s_coverMasked;
extern bool s_coverSprInited;
extern bool s_coverSprReady;
extern LGFX_Sprite s_coverCacheSpr0;
extern LGFX_Sprite s_coverCacheMasked0;
extern LGFX_Sprite s_coverCacheSpr1;
extern LGFX_Sprite s_coverCacheMasked1;
extern LGFX_Sprite* s_coverCacheSpr[2];
extern LGFX_Sprite* s_coverCacheMasked[2];
extern bool s_coverCacheInited;
extern bool s_coverCacheReady[2];
extern int s_coverCacheTrackIdx[2];

/* 旋转/遮罩/临时帧缓冲。 */
extern LGFX_Sprite s_frame0;
extern LGFX_Sprite s_frame1;
extern LGFX_Sprite* s_frame[2];
extern uint8_t s_front;
extern uint8_t s_back;
extern bool s_framesInited;
extern LGFX_Sprite s_rotFrame0;
extern LGFX_Sprite s_rotFrame1;
extern LGFX_Sprite* s_rotFrame[2];
extern uint8_t s_rotFront;
extern uint8_t s_rotBack;
extern bool s_rotFramesInited;
extern LGFX_Sprite* s_src;

extern int s_list_last_drawn_idx;
extern float s_angle_deg;
extern uint32_t s_rot_last_ms;
extern bool s_rotate_wait_audio_start;
extern uint32_t s_cover_apply_ms;
extern uint32_t s_rotate_release_ms;
extern uint32_t s_rotate_release_audio_ms;
extern int s_rotate_probe_frames_left;

extern uint32_t s_scan_last_ms;
extern int s_scan_phase;
extern volatile bool s_ui_hold;

/* UiTask / SPI 访问协调。 */
void ui_lock();
void ui_unlock();
void ui_draw_lock();
void ui_draw_unlock();
void ui_request_refresh();

/* UI 内部页面接口：仅供 ui_pages.cpp 或 UI 内部路径使用，不暴露给业务层。 */
void ui_show_message(const char* msg);
void ui_enter_boot(void);
void ui_show_scanning();
void ui_clear_screen();

/* 播放页绘图与封面管线内部接口。 */
void cover_sprite_init_once();
void cover_cache_sprite_init_once();
void cover_frames_init_once();
bool cover_decode_to_sprite_from_track(const TrackInfo& t);
void cover_rotate_draw(float angle_deg);
void cover_panel_draw(float angle_deg);
void cover_info_draw();
// 面板页 bilinear 源封面缓存失效：s_coverSpr 内容变化后必须调用，避免切歌仍显示旧封面。
void cover_panel_invalidate_source_cache();

/* NFC 绑定类型弹窗：播放器无封面/未开播占位页也需要能绘制。 */
bool ui_nfc_bind_target_popup_is_visible();
bool ui_nfc_bind_target_popup_consume_dirty();
void ui_draw_nfc_bind_target_popup_on_tft_if_visible();

/* NFC 刷卡结果弹窗：播放器无封面/未开播占位页也需要能绘制。 */
bool ui_nfc_scan_popup_is_visible();
bool ui_nfc_scan_popup_consume_dirty();
void ui_draw_nfc_scan_popup_on_tft_if_visible();

/* 电池状态页脚绘制。 */
void ui_draw_battery_footer(LGFX_Sprite* dst);
