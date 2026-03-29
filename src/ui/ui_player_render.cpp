#include <Arduino.h>
#include "ui/ui_internal.h"
#include "ui/ui_text_utils.h"
#include "ui/ui_progress.h"
#include "ui/ui_colors.h"
#include "ui/ui_icons.h"
#include "audio/audio.h"
#include "lyrics/lyrics.h"
#include "utils/log.h"
#undef LOG_TAG
#define LOG_TAG "UI"

static void cover_set_source(LGFX_Sprite* src)
{
  s_src = src;
}

// 将旋转的封面渲染到后帧并推送到 LCD（稳定路径）
// 参数: angle_deg - 旋转角度（度）
// 功能: 将源精灵旋转指定角度后绘制到后帧，然后推送到屏幕
//      使用双缓冲技术避免闪烁，交换前后帧索引
void cover_rotate_draw(float angle_deg)
{
  // 检查旋转帧是否已初始化以及源精灵是否有效
  if (!s_rotFramesInited || !s_src) return;

  // 获取后帧指针（旋转专用双缓冲）
  auto* dst = s_rotFrame[s_rotBack];
  // 清空后帧
  dst->fillScreen(TFT_BLACK);
  // 将源精灵旋转指定角度并绘制到后帧（不缩放）
  s_src->pushRotateZoom(dst, COVER_SIZE / 2, COVER_SIZE / 2, angle_deg, 1.0f, 1.0f);

  // 将后帧推送到屏幕 (0, 0) 位置
  dst->pushSprite(0, 0);

  // 交换前后帧索引（双缓冲）
  uint8_t tmp = s_rotFront;
  s_rotFront = s_rotBack;
  s_rotBack = tmp;
}

/**
 * 基于时间的推移式滚动
 * @param progress: 当前句播放进度 (0.0 到 1.0)
 */
static void draw_scrolling_line_by_time(LGFX_Sprite* dst, const char* text, int y, 
                                       int safe_pad, uint16_t color, float progress)
{
  if (!text || strlen(text) == 0) return;

  dst->setTextColor(color);
  dst->setFont(&g_font_cjk);
  int text_w = dst->textWidth(text);
  int available_w = 240 - (safe_pad * 2);

  if (text_w <= available_w) {
    draw_center_text_on_sprite(dst, text, y, color, safe_pad);
    return;
  }

  // 计算最大可滚动距离
  int max_scroll = text_w - available_w;

  // --- 核心逻辑：进度映射 ---
  // 我们让进度在 10% 到 90% 的时间内进行滚动，前后留出时间让用户看清头尾
  float scroll_factor = 0;
  if (progress < 0.1f) {
    scroll_factor = 0; // 头部静止
  } else if (progress > 0.9f) {
    scroll_factor = 1.0f; // 尾部静止（刚好贴在右侧）
  } else {
    // 中间 80% 的时间用来平滑移动
    scroll_factor = (progress - 0.1f) / 0.8f;
  }

  int current_offset = (int)(max_scroll * scroll_factor);

  // 裁剪并绘制
  // 裁剪区域高度设置为 26，确保完整显示当前行文字（包含字体的上伸/下伸部分）
  dst->setClipRect(safe_pad, y - 11, available_w, 26);
  dst->setCursor(safe_pad - current_offset, y);
  dst->print(text);
  dst->clearClipRect();
}

void cover_info_draw()
{
  if (!s_framesInited) return;

  uint32_t t0 = millis();

  auto* dst = s_frame[s_back];
  dst->fillScreen(TFT_BLACK);

  uint32_t t1 = millis();

  // 1) 静态封面（整屏）- 使用带遮罩的版本
  if (s_coverSprReady) {
    s_coverMasked.pushSprite(dst, 0, 0);
  }
  uint32_t t_cover = millis();

  // 2) 参数：圆屏安全边距 + 更大的字号 + 更紧凑的排布
  const int safe_pad = 12;

  const uint16_t c_title  = UI_COLOR_TITLE;   // 歌名文字颜色（纯白）
  const uint16_t c_artist = UI_COLOR_ARTIST;  // 歌手文字颜色（浅灰）
  const uint16_t c_lyrics = 0xFFFF;           // 歌词颜色（白色）
  const uint16_t c_lyrics_next = 0xAD55;      // 下一句歌词颜色（亮灰）

  // 3) 把信息区抬高一点，避免圆屏底部变窄导致左右被裁

  const int y_status = 131;  // 状态栏（音量/模式/列表）上移1像素
  const int y_bar   = 149;   // 进度条下移1像素
  const int y_time  = 157;   // 时间（上移3像素）
  const int y_title = 176;   // 标题
  const int y_artist= 195;   // 歌手（下移3像素）

  // 4) 歌词显示（屏幕上半部分）- 在遮罩之后绘制，确保可见
  bool hasLyrics = g_lyricsDisplay.hasLyrics();
  LOGD("[UI] hasLyrics: %d", hasLyrics);
  if (hasLyrics) {
    // 使用滚动歌词显示（3行：上一句、当前、下一句）
    LyricsDisplay::ScrollLyrics scroll = g_lyricsDisplay.getScrollLyrics();
    
    // 歌词显示位置 - 使用 constexpr 便于编译器优化
    static constexpr int Y_LYRICS_CENTER = 93;   // 中心位置
    static constexpr int LINE_HEIGHT = 20;       // 行高
    static constexpr int ANIM_END_PERCENT = 80;  // 前80%完成动画
    
    // 预计算缓动表（0-100 映射到 0-100 的 ease-out 曲线）
    // ease-out: y = 1 - (1-x)^2，展开后避免 pow 调用
    static const uint8_t ease_table[101] = {
      0,  2,  4,  6,  8, 10, 12, 14, 15, 17, 19, 21, 23, 24, 26, 28,
     30, 31, 33, 35, 36, 38, 40, 41, 43, 44, 46, 47, 49, 50, 52, 53,
     55, 56, 58, 59, 60, 62, 63, 64, 66, 67, 68, 70, 71, 72, 73, 75,
     76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91,
     91, 92, 93, 94, 94, 95, 96, 96, 97, 97, 98, 98, 99, 99, 99,100,
    100,100,100,100,100,100,100,100,100,100,100,100,100,100,100,100,
    100,100,100,100,100
    };
    
    // 将进度转换为 0-100 的整数索引
    int progress_int = (int)(scroll.progress * 100.0f);
    if (progress_int > 100) progress_int = 100;
    if (progress_int < 0) progress_int = 0;
    
    // 计算动画进度（前80%完成动画）
    int anim_progress_int;
    if (progress_int < ANIM_END_PERCENT) {
      anim_progress_int = (progress_int * 100) / ANIM_END_PERCENT;
    } else {
      anim_progress_int = 100;
    }
    // 安全检查：确保不越界
    if (anim_progress_int > 100) anim_progress_int = 100;
    if (anim_progress_int < 0) anim_progress_int = 0;

    // 查表获取缓动值，计算偏移（避免浮点运算）
    int offset = (ease_table[anim_progress_int] * LINE_HEIGHT) / 100;
    
    // 绘制上一句（灰色，淡出效果）
    if (scroll.prev && scroll.prev[0] != '\0') {
      draw_center_text_on_sprite(dst, scroll.prev, 
                                Y_LYRICS_CENTER - LINE_HEIGHT - offset, 
                                c_lyrics_next, safe_pad);
    }
    
    // 绘制当前句（白色，高亮，支持基于时间的滚动）
    if (scroll.current && scroll.current[0] != '\0') {
      draw_scrolling_line_by_time(dst, scroll.current, 
                                Y_LYRICS_CENTER - offset, 
                                safe_pad, c_lyrics, scroll.progress);
    }
    
    // 绘制下一句（灰色，淡入效果）
    if (scroll.next && scroll.next[0] != '\0') {
      draw_center_text_on_sprite(dst, scroll.next, 
                                Y_LYRICS_CENTER + LINE_HEIGHT - offset, 
                                c_lyrics_next, safe_pad);
    }
  }

  uint32_t t_lyrics = millis();

  // 时间显示颜色常量
  const uint16_t c_time_text = UI_COLOR_TIME;  // 时间文字颜色（浅灰）

  // 4) 进度条（显示 elapsed + total）
  uint32_t el_ms = audio_get_play_ms();
  uint32_t total_ms = audio_get_total_ms();

  char total_str[6];
  if (total_ms >= 1000 && total_ms != 0xFFFFFFFFu) {
    fmt_mmss(total_ms, total_str);
  } else {
    memcpy(total_str, "--:--", 6); // 包含 '\0'
  }

  // 检查音量是否处于激活状态
  bool volume_active = (millis() - s_ui_volume_active_time) < VOLUME_ACTIVE_TIMEOUT_MS;
  draw_status_row(dst, y_status, safe_pad, c_time_text, volume_active);

  draw_time_bar(dst,
                y_bar, y_time,
                el_ms,
                total_ms,
                safe_pad,
                c_time_text);

  uint32_t t_status = millis();

  // 5) 标题/歌手（支持滚动显示长文本）
  extern void draw_note_icon_img(LGFX_Sprite* dst, int x, int y, uint16_t color);
  extern void draw_artist_icon_img(LGFX_Sprite* dst, int x, int y, uint16_t color);
  
  // 更新滚动偏移（像素滚动，30ms间隔）
  uint32_t now = millis();
  if (now - s_scroll_last_ms > 30) {
    s_scroll_last_ms = now;
    
    // 标题滚动
    bool title_scroll = draw_scrolling_text_with_icon(dst, y_title, s_np_title, s_title_scroll_x, 
                                                       14, c_title, safe_pad, draw_note_icon_img);
    if (title_scroll) {
      s_title_scroll_x += SCROLL_SPEED;
      // 滚动范围：文本宽度 + 间距
      extern lgfx::U8g2font g_font_cjk;
      dst->setFont(&g_font_cjk);
      int title_w = dst->textWidth(s_np_title.c_str());
      if (s_title_scroll_x > title_w + SCROLL_GAP) {
        s_title_scroll_x = 0;
      }
    } else {
      s_title_scroll_x = 0;
    }
    
    // 歌手滚动
    bool artist_scroll = draw_scrolling_text_with_icon(dst, y_artist, s_np_artist, s_artist_scroll_x,
                                                        14, c_artist, safe_pad, draw_artist_icon_img);
    if (artist_scroll) {
      s_artist_scroll_x += SCROLL_SPEED;
      extern lgfx::U8g2font g_font_cjk;
      dst->setFont(&g_font_cjk);
      int artist_w = dst->textWidth(s_np_artist.c_str());
      if (s_artist_scroll_x > artist_w + SCROLL_GAP) {
        s_artist_scroll_x = 0;
      }
    } else {
      s_artist_scroll_x = 0;
    }
  } else {
    // 使用当前偏移绘制
    draw_scrolling_text_with_icon(dst, y_title, s_np_title, s_title_scroll_x, 
                                  14, c_title, safe_pad, draw_note_icon_img);
    draw_scrolling_text_with_icon(dst, y_artist, s_np_artist, s_artist_scroll_x,
                                  14, c_artist, safe_pad, draw_artist_icon_img);
  }

  uint32_t t_text = millis();

  // 6) 推屏
  dst->pushSprite(0, 0);

  uint32_t t_push = millis();

  uint8_t tmp = s_front;
  s_front = s_back;
  s_back  = tmp;
}

bool ui_draw_cover_for_track(const TrackInfo& t, bool force_redraw)
{
  static String last_sig;
  String sig = t.audio_path + "#" + String((unsigned)t.cover_offset) + "#" + String((unsigned)t.cover_size);
  if (!force_redraw && sig == last_sig) return true;
  last_sig = sig;

  bool ok = cover_decode_to_sprite_from_track(t);
  if (!ok) return false;

  ui_draw_lock();
  cover_set_source(&s_coverSpr);
  s_angle_deg = 0.0f;
  s_rot_last_ms = 0;   // 让 UiTask 下次自己初始化 dt

  // 切歌：先清空总时长，等待播放层重新喂入
  s_ui_play_ms  = 0;
  s_ui_total_ms = 0;

  // 立即推送第一帧
  if (!s_screen_cleared) {
    tft.fillScreen(TFT_BLACK);
    s_screen_cleared = true;
  }
  s_coverSpr.pushSprite(0, 0);

  s_angle_deg = 0.0f;
  s_rot_last_ms = millis();
  ui_draw_unlock();
  ui_request_refresh();

  return true;
}

// =============================================================================
// 可选的覆盖层 API（目前保持为轻量级存根）
// =============================================================================
void ui_player_draw_overlay(const TrackInfo&, uint32_t, uint32_t,
                            const char*, const char*, const char*)
{
  // 最小化构建：无覆盖层
}

void ui_player_update_progress(uint32_t play_ms, uint32_t total_ms)
{
  s_ui_play_ms  = play_ms;
  s_ui_total_ms = total_ms;   // 0 表示未知

  // 更新歌词显示时间
  g_lyricsDisplay.updateTime(play_ms);
  ui_request_refresh();
}

void ui_player_update_lyrics(const char*, const char*)
{
  // 最小化构建：无覆盖层
}


enum ui_player_view_t ui_get_view() { return s_view; }

// 设置播放器视图（带线程锁保护）
// 参数: new_view - 新视图类型（ROTATE旋转封面 或 INFO信息详情）
// 功能: 更新视图状态，重置旋转时间戳防止角度跳变
static inline void ui_set_view(ui_player_view_t new_view)
{
  const uint32_t now_ms = millis();
  ui_lock();
  s_view = new_view;
  s_rot_last_ms = now_ms;   // 防 dt 累积跳角度
  s_rotate_wait_audio_start = false;
  s_rotate_wait_prefetch_done = false;
  ui_unlock();
  ui_request_refresh();
}

// 切换播放器视图（长按PLAY键触发）
// 在旋转视图(ROTATE)和信息视图(INFO)之间切换
void ui_toggle_view()
{
  LOGI("[UI] toggle_view: current=%d", (int)s_view);
  ui_set_view((s_view == UI_VIEW_ROTATE) ? UI_VIEW_INFO : UI_VIEW_ROTATE);
  LOGI("[UI] toggle_view: new=%d", (int)s_view);
}

void ui_set_now_playing(const char* title, const char* artist)
{
  ui_lock();
  s_np_title  = title  ? String(title)  : String("");
  s_np_artist = artist ? String(artist) : String("");
  // 切歌时重置滚动偏移
  s_title_scroll_x = 0;
  s_artist_scroll_x = 0;
  s_scroll_last_ms = 0;
  ui_unlock();
  ui_request_refresh();
}

void ui_set_album(const String& album)
{
  ui_lock();
  s_np_album = album;
  ui_unlock();
  // 切歌时重置专辑滚动偏移
  reset_album_scroll();
  ui_request_refresh();
}

void ui_set_volume(uint8_t vol)
{
  if (vol > 100) vol = 100;
  s_ui_volume = vol;
  ui_request_refresh();
}

void ui_volume_key_pressed()
{
  s_ui_volume_active_time = millis();  // 更新音量激活时间
  ui_request_refresh();
}

void ui_set_play_mode(play_mode_t mode)
{
  s_ui_play_mode = mode;
  ui_request_refresh();
}

void ui_mode_switch_highlight()
{
  s_ui_mode_switch_time = millis();  // 记录模式切换时间，用于高亮显示
  ui_request_refresh();
}

void ui_set_track_pos(int idx, int total)
{
  if (total < 0) total = 0;
  if (idx < 0) idx = 0;
  s_ui_track_idx = idx;
  s_ui_track_total = total;
  ui_request_refresh();
}
