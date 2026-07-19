#include <Arduino.h>
#include <driver/gpio.h>
#include "keys/keys.h"
#include "keys/keys_pins.h"

#include "app_flags.h"
#include "app_state.h"
#include "app_power.h"
#include "ui/ui.h"
#include "player_control.h"
#include "player_list_select.h"
#include "nfc/nfc_admin_state.h"
#include "menu/quick_menu.h"
#include "menu/quick_menu_page_nfc.h"
#include "utils/log.h"
#include "web/web_server.h"
#include "web/web_settings.h"
#include "board/board_pins_pcb1_mcp23017.h"
#include "hal/mcp23017_u3.h"
#include "hal/board_hw_control.h"


/*
 * 按键输入模块。
 *
 * 当前 MODE 键的语义：
 * - 普通播放页：短按切换音量步进模式（X1 / X5），长按切换播放界面视图
 * - 列表页 / 快捷菜单 / NFC 管理页：作为返回、退出或取消键
 * - 扫描状态：作为取消扫描键
 */

static inline bool pressed(int level) { return level == LOW; } // 按下接地

struct KeyCtx {
  int pin;
  int last;
  uint32_t t_down;
  bool long_fired;
  uint32_t t_repeat;
};

static KeyCtx k_mode  { PIN_KEY_MODE,  HIGH, 0, false, 0 };
static KeyCtx k_play  { PIN_KEY_PLAY,  HIGH, 0, false, 0 };
static KeyCtx k_prev  { PIN_KEY_PREV,  HIGH, 0, false, 0 };
static KeyCtx k_next  { PIN_KEY_NEXT,  HIGH, 0, false, 0 };
static KeyCtx k_ec06e { PIN_KEY_MCP_EC06_E, HIGH, 0, false, 0 };
static KeyCtx k_voldn { PIN_KEY_VOLDN, HIGH, 0, false, 0 };
static KeyCtx k_volup { PIN_KEY_VOLUP, HIGH, 0, false, 0 };

// VOLDN 双击检测
static bool s_voldn_click_pending = false;
static uint32_t s_voldn_click_deadline = 0;
static constexpr uint32_t VOLDN_DOUBLE_CLICK_MS = 320;

static bool s_rescan_cancel_armed = false;

// EC06 旋钮相关
static int s_enc_last = 0;
static int s_enc_accum = 0;
static uint32_t s_enc_last_step_ms = 0;

// MCP23017 GPIOA 包含 4 个按键和 BT_LINK。按键扫描每轮只允许读取一次端口，
// 不能为每一个按键分别发起 I2C 事务。
static constexpr uint32_t MCP_KEY_SCAN_INTERVAL_MS = 10;
static constexpr uint32_t MCP_KEY_RETRY_INTERVAL_MS = 100;
static uint8_t s_mcp_a_cache = 0xFF;
static bool s_mcp_a_cache_valid = false;
static uint32_t s_mcp_a_cache_ms = 0;

// EC06：12 定位 / 6 脉冲。
// 这类编码器通常一格定位对应 2 个有效 quadrature 边沿。
// 如果用 4 个边沿算一步，会变成转两格才触发一次。
static constexpr int ENCODER_EDGES_PER_STEP = 2;

// 旋钮单步防抖时间。原来 30ms 偏保守，12定位/6脉冲可适当降低。
// 如果后续发现快速旋转漏步，可以再降到 10ms；如果误触发，升回 30ms。
static constexpr uint32_t ENCODER_STEP_GUARD_MS = 15;

static int read_encoder_state()
{
    const int a = digitalRead(PIN_EC06_A);
    const int b = digitalRead(PIN_EC06_B);
    return (a << 1) | b;
}

static int8_t decode_encoder_delta(int last_state, int now_state)
{
    const int transition = (last_state << 2) | now_state;

    switch (transition) {
        // 一个方向
        case 0b0001:
        case 0b0111:
        case 0b1110:
        case 0b1000:
            return +1;

        // 反方向
        case 0b0010:
        case 0b1011:
        case 0b1101:
        case 0b0100:
            return -1;

        default:
            return 0;
    }
}

static int8_t read_encoder_step()
{
    const int now_state = read_encoder_state();

    if (now_state == s_enc_last) {
        return 0;
    }

    const int8_t delta = decode_encoder_delta(s_enc_last, now_state);
    s_enc_last = now_state;

    if (delta == 0) {
        return 0;
    }

    s_enc_accum += delta;

    // EC06 常见一格会产生多个边沿，累计到阈值再触发一次。
    if (s_enc_accum >= ENCODER_EDGES_PER_STEP) {
        s_enc_accum = 0;

        const uint32_t now = millis();
        if (now - s_enc_last_step_ms < ENCODER_STEP_GUARD_MS) {
            return 0;
        }
        s_enc_last_step_ms = now;

        return -1;
    }

    if (s_enc_accum <= -ENCODER_EDGES_PER_STEP) {
        s_enc_accum = 0;

        const uint32_t now = millis();
        if (now - s_enc_last_step_ms < ENCODER_STEP_GUARD_MS) {
            return 0;
        }
        s_enc_last_step_ms = now;

        return +1;
    }

    return 0;
}

static constexpr int ENCODER_VOLUME_STEP = 1;
static constexpr int ENCODER_VOLUME_FAST_STEP = 5;
static constexpr uint32_t VOLUME_FAST_MODE_TIMEOUT_MS = 5000;

static bool s_volume_fast_mode = false;
static uint32_t s_volume_fast_last_ms = 0;

// 正常播放器页的 PREV/NEXT 长按状态。
// 普通长按用于快退/快进；编码器先按住时，长按 PREV/NEXT 进入 NFC/列表组合功能。
static constexpr uint32_t PLAYER_COMBO_HOLD_MS = 600;
static constexpr uint32_t PLAYER_SEEK_HOLD_MS = 650;
static constexpr uint32_t PLAYER_SEEK_REPEAT_MS = 400;
static constexpr uint32_t PLAYER_SEEK_STEP_MS = 10000;
static constexpr uint32_t PLAYER_ENCODER_LONG_MS = 800;

enum class PlayerNavHoldAction : uint8_t {
    None = 0,
    SeekPrev,
    SeekNext,
    UnavailablePrev,
    UnavailableNext,
};

static PlayerNavHoldAction s_player_nav_hold_action = PlayerNavHoldAction::None;
static bool s_prev_combo_armed = false;
static bool s_next_combo_armed = false;
static bool s_seek_preview_visible = false;
static uint32_t s_seek_preview_target_ms = 0;
static uint32_t s_seek_preview_total_ms = 0;
static uint32_t s_seek_preview_revision = 0;
static uint32_t s_seek_preview_next_repeat_ms = 0;
static uint32_t s_seek_preview_auto_hide_ms = 0;

static void volume_fast_mode_enter()
{
    s_volume_fast_mode = true;
    s_volume_fast_last_ms = millis();
    ui_show_volume_step_hint(ENCODER_VOLUME_FAST_STEP);
}

static void volume_fast_mode_exit()
{
    s_volume_fast_mode = false;
    ui_show_volume_step_hint(ENCODER_VOLUME_STEP);
}

static void volume_fast_mode_toggle()
{
    // MODE 短按的语义：当前是有效 X5 时切回 X1；
    // 如果 X5 已经超时自动退出，下一次短按应该重新进入 X5。
    // 不能只看 s_volume_fast_mode，因为超时后如果没有再转动旋钮，
    // 内部标志可能还保持 true，导致短按被误认为“从 X5 切回 X1”。
    if (s_volume_fast_mode) {
        const bool still_active = (millis() - s_volume_fast_last_ms) <= VOLUME_FAST_MODE_TIMEOUT_MS;
        if (still_active) {
            volume_fast_mode_exit();
            return;
        }

        // 已超时：只修正内部状态，不再额外显示一次 X1，随后直接进入 X5。
        s_volume_fast_mode = false;
    }

    volume_fast_mode_enter();
}

static bool volume_fast_mode_is_active()
{
    if (!s_volume_fast_mode) {
        return false;
    }

    if (millis() - s_volume_fast_last_ms > VOLUME_FAST_MODE_TIMEOUT_MS) {
        volume_fast_mode_exit();
        return false;
    }

    return true;
}

static int current_encoder_volume_step()
{
    return volume_fast_mode_is_active()
        ? ENCODER_VOLUME_FAST_STEP
        : ENCODER_VOLUME_STEP;
}

// NFC 弹窗会复用通用按键处理函数；这里提前声明，函数体在后面。
static void handle_key(KeyCtx& k,
                       void (*on_short)(),
                       void (*on_long)(),
                       bool repeat,
                       void (*on_repeat)());

// =============================================================================
// NFC 绑定类型小弹窗
// =============================================================================

static constexpr uint32_t NFC_BIND_POPUP_TIMEOUT_MS = 10000;
static constexpr uint8_t NFC_BIND_POPUP_OPTION_COUNT = 3;

static bool s_nfc_bind_popup_active = false;
static uint8_t s_nfc_bind_popup_selected = 0; // 0=单曲，1=歌手，2=专辑
static uint32_t s_nfc_bind_popup_last_ms = 0;

static uint8_t nfc_bind_default_selection()
{
    // 播放模式的 6 个值已经在 ui.h 里定义，这里读取统一快照后按枚举判断，
    // 不依赖 player_playlist.cpp 里的辅助函数，减少按键模块的耦合。
    switch (app_play_mode_get()) {
        case PLAY_MODE_ARTIST_SEQ:
        case PLAY_MODE_ARTIST_RND:
            return 1;

        case PLAY_MODE_ALBUM_SEQ:
        case PLAY_MODE_ALBUM_RND:
            return 2;

        case PLAY_MODE_ALL_SEQ:
        case PLAY_MODE_ALL_RND:
        default:
            return 0;
    }
}

static void nfc_bind_popup_touch()
{
    s_nfc_bind_popup_last_ms = millis();
}

static void nfc_bind_popup_close()
{
    if (!s_nfc_bind_popup_active) {
        return;
    }

    s_nfc_bind_popup_active = false;
    ui_hide_nfc_bind_target_popup();
}

static void nfc_bind_popup_open()
{
    // 打开 NFC 选择弹窗后，关闭 X5 音量模式，避免两个中心弹窗互相打架。
    s_volume_fast_mode = false;
    s_nfc_bind_popup_active = true;
    s_nfc_bind_popup_selected = nfc_bind_default_selection();
    nfc_bind_popup_touch();
    ui_show_nfc_bind_target_popup(s_nfc_bind_popup_selected);

    // 组合键触发弹窗后，消费当前按键电平，避免松手又触发快捷菜单或上一曲。
    keys_sync_to_hw_state();
}

static void nfc_bind_popup_move(int8_t delta)
{
    if (!s_nfc_bind_popup_active || delta == 0) {
        return;
    }

    int next = static_cast<int>(s_nfc_bind_popup_selected) + (delta > 0 ? 1 : -1);
    if (next < 0) {
        next = NFC_BIND_POPUP_OPTION_COUNT - 1;
    } else if (next >= NFC_BIND_POPUP_OPTION_COUNT) {
        next = 0;
    }

    s_nfc_bind_popup_selected = static_cast<uint8_t>(next);
    nfc_bind_popup_touch();
    ui_show_nfc_bind_target_popup(s_nfc_bind_popup_selected);
}

static void nfc_bind_popup_confirm()
{
    if (!s_nfc_bind_popup_active) {
        return;
    }

    nfc_bind_popup_touch();

    bool ok = false;
    switch (s_nfc_bind_popup_selected) {
        case 0:
            ok = quick_menu_nfc_bind_current_track();
            break;
        case 1:
            ok = quick_menu_nfc_bind_current_artist();
            break;
        case 2:
            ok = quick_menu_nfc_bind_current_album();
            break;
        default:
            break;
    }

    if (ok) {
        s_nfc_bind_popup_active = false;
        ui_hide_nfc_bind_target_popup();
    } else {
        // 失败通常是当前播放源不是本地曲库、曲库未就绪或当前歌曲没有对应分组。
        // 先保留弹窗，方便用户改选其它绑定类型。
        ui_show_nfc_bind_target_popup(s_nfc_bind_popup_selected);
    }

    keys_sync_to_hw_state();
}

static bool nfc_bind_popup_handle_if_active(int8_t encoder_step)
{
    if (!s_nfc_bind_popup_active) {
        return false;
    }

    const uint32_t now = millis();
    if (now - s_nfc_bind_popup_last_ms > NFC_BIND_POPUP_TIMEOUT_MS) {
        nfc_bind_popup_close();
        return true;
    }

    // 弹窗打开时，旋钮/上一曲/下一曲只负责选择绑定类型，不再切歌或调音量。
    if (encoder_step > 0) {
        nfc_bind_popup_move(+1);
    } else if (encoder_step < 0) {
        nfc_bind_popup_move(-1);
    }

    handle_key(k_prev, [](){ nfc_bind_popup_move(-1); }, nullptr, false, nullptr);
    handle_key(k_next, [](){ nfc_bind_popup_move(+1); }, nullptr, false, nullptr);
    handle_key(k_mode, nfc_bind_popup_close, nullptr, false, nullptr);
    handle_key(k_ec06e, nfc_bind_popup_confirm, nullptr, false, nullptr);
    handle_key(k_play, nfc_bind_popup_confirm, nullptr, false, nullptr);

    return true;
}

static void handle_encoder_volume_step(int8_t step)
{
    if (step == 0) {
        return;
    }

    const int volume_step = current_encoder_volume_step();

    if (s_volume_fast_mode) {
        s_volume_fast_last_ms = millis();
    }

    ui_volume_key_pressed();
    player_volume_step(step > 0 ? volume_step : -volume_step);
    ui_show_volume_step_hint(static_cast<uint8_t>(volume_step));
}

static void play_key_toggle_with_solenoid()
{
    // 播放键短按时，可选给 TC118S 输出一次电磁铁翻转短脉冲。
    // 只在播放键语义里触发；菜单确认、NFC确认、HALL触发不走这里。
    if (web_settings_get().solenoid_enabled) {
        (void)board_hw_solenoid_flip();
    }
    player_toggle_play(PlayerToggleTrigger::PlayKey);
}

static void enter_quick_menu_from_player()
{
    volume_fast_mode_exit();
    quick_menu_enter();
    ui_request_refresh_now();

    // 进入快捷菜单后立即同步一次按键状态。
    // 这样本轮按键扫描后半段不会继续按“播放器页”语义处理 PREV/NEXT，
    // 可避免开机第一次进菜单时残留按键边沿被误当成切歌。
    keys_sync_to_hw_state();
}

static void quick_menu_key_and_refresh(QuickMenuKey key)
{
    quick_menu_handle_key(key);
    ui_request_refresh_now();
}

static void list_select_key_and_refresh(key_event_t evt)
{
    player_list_select_handle_key(evt);
    ui_request_refresh_now();
}

static bool refresh_mcp_a_cache(bool force = false)
{
    const uint32_t now = millis();
    const uint32_t interval_ms = s_mcp_a_cache_valid
        ? MCP_KEY_SCAN_INTERVAL_MS
        : MCP_KEY_RETRY_INTERVAL_MS;

    if (!force && s_mcp_a_cache_ms != 0 && now - s_mcp_a_cache_ms < interval_ms) {
        return s_mcp_a_cache_valid;
    }

    s_mcp_a_cache_ms = now;

    uint8_t value = 0xFF;
    if (!mcp23017_u3_read_port_a(&value)) {
        s_mcp_a_cache = 0xFF;
        s_mcp_a_cache_valid = false;
        return false;
    }

    s_mcp_a_cache = value;
    s_mcp_a_cache_valid = true;
    return true;
}

static int read_mcp_a_active_low(uint8_t bit)
{
    if (!s_mcp_a_cache_valid) {
        return HIGH;
    }

    return (s_mcp_a_cache & (1u << bit)) ? HIGH : LOW;
}

static int read_key_pin(int pin)
{
    switch (pin) {
        case PIN_KEY_DISABLED:
            return HIGH;

        case PIN_KEY_MCP_BACK_MODE:
            return read_mcp_a_active_low(board::MCP_A_KEY_BACK_MODE);

        case PIN_KEY_MCP_EC06_E:
            return read_mcp_a_active_low(board::MCP_A_EC06_E);

        case PIN_KEY_MCP_PREV_NFC:
            return read_mcp_a_active_low(board::MCP_A_KEY_PREV_NFC);

        case PIN_KEY_MCP_NEXT_LIST:
            return read_mcp_a_active_low(board::MCP_A_KEY_NEXT_LIST);

        default:
            if (pin < 0) return HIGH;
            return digitalRead(pin);
    }
}

static void setup_key_pin(int pin)
{
    if (pin >= 0) {
        pinMode(pin, INPUT_PULLUP);
    }
}

/* VOLDN 双击提交：切换 WiFi */
static void voldn_click_commit_double()
{
  web_wifi_toggle();
  LOGW("[应用] WiFi 已切换：%s", web_wifi_is_enabled() ? "开启" : "关闭");
  s_voldn_click_pending = false;
  s_voldn_click_deadline = 0;
}

static void handle_key(KeyCtx& k,
                       void (*on_short)(),
                       void (*on_long)(),
                       bool repeat = false,
                       void (*on_repeat)() = nullptr)
{
  uint32_t now = millis();
  int s = read_key_pin(k.pin);

  // 边沿检测
  if (s != k.last) {
    k.last = s;
    if (pressed(s)) {
      k.t_down = now;
      k.long_fired = false;
      k.t_repeat = now;
      // 音量按键按下时立即通知UI
      if (repeat) ui_volume_key_pressed();
    } else {
      // 松开：短按触发
      if (!k.long_fired && (now - k.t_down) > 25) {
        if (on_short) on_short();
      }
    }
  }

  // 长按触发一次
  if (pressed(k.last) && !k.long_fired && (now - k.t_down) >= 800) {
    k.long_fired = true;
    if (on_long) on_long();
  }

  // 按住连发（音量）
  // ✅ 渐进式连发：按住时间越长，音量变动越快
  if (repeat && pressed(k.last) && on_repeat) {
    uint32_t hold_time = now - k.t_down;
    uint32_t repeat_interval = 150; // 默认 150ms 间隔

    // 按住超过 2 秒后加速到 50ms 间隔
    if (hold_time > 2000) {
      repeat_interval = 50;
    }

    if (now - k.t_repeat >= repeat_interval) {
      k.t_repeat = now;
      on_repeat();
    }
  }

  // ✅ 防止长时间按键扫描逻辑阻塞系统
  yield();
}

enum class KeyEdge : uint8_t {
  None = 0,
  Pressed,
  Released,
};

static KeyEdge update_key_edge(KeyCtx& key, int level, uint32_t now)
{
  if (level == key.last) {
    return KeyEdge::None;
  }

  key.last = level;
  if (pressed(level)) {
    key.t_down = now;
    key.long_fired = false;
    key.t_repeat = now;
    return KeyEdge::Pressed;
  }

  return KeyEdge::Released;
}

static void player_nav_hold_reset(bool hide_preview)
{
  s_player_nav_hold_action = PlayerNavHoldAction::None;
  s_prev_combo_armed = false;
  s_next_combo_armed = false;
  s_seek_preview_target_ms = 0;
  s_seek_preview_total_ms = 0;
  s_seek_preview_revision = 0;
  s_seek_preview_next_repeat_ms = 0;
  s_seek_preview_auto_hide_ms = 0;

  if (hide_preview && s_seek_preview_visible) {
    s_seek_preview_visible = false;
    ui_hide_seek_preview();
  }
}

static uint32_t player_seek_step_target(uint32_t target_ms,
                                        uint32_t total_ms,
                                        bool forward)
{
  const uint32_t max_target_ms = total_ms > 500 ? total_ms - 500 : 0;
  if (!forward) {
    return target_ms > PLAYER_SEEK_STEP_MS
        ? target_ms - PLAYER_SEEK_STEP_MS
        : 0;
  }

  if (target_ms >= max_target_ms ||
      max_target_ms - target_ms <= PLAYER_SEEK_STEP_MS) {
    return max_target_ms;
  }
  return target_ms + PLAYER_SEEK_STEP_MS;
}

static void player_seek_preview_begin(bool forward, uint32_t now)
{
  PlayerSeekWindow window{};
  if (!player_seek_window_get(&window)) {
    s_player_nav_hold_action = forward
        ? PlayerNavHoldAction::UnavailableNext
        : PlayerNavHoldAction::UnavailablePrev;
    s_seek_preview_visible = true;
    ui_show_seek_unavailable();
    LOGW("[按键] %s不可用：当前音源不支持跳转",
         forward ? "快进" : "快退");
    return;
  }

  s_player_nav_hold_action = forward
      ? PlayerNavHoldAction::SeekNext
      : PlayerNavHoldAction::SeekPrev;
  s_seek_preview_total_ms = window.total_ms;
  s_seek_preview_revision = window.playback_revision;
  s_seek_preview_target_ms = player_seek_step_target(window.current_ms,
                                                     window.total_ms,
                                                     forward);
  s_seek_preview_next_repeat_ms = now + PLAYER_SEEK_REPEAT_MS;
  s_seek_preview_visible = true;
  ui_show_seek_preview(forward ? +1 : -1, s_seek_preview_target_ms);

  LOGD("[按键] 开始%s预览：当前=%lums 目标=%lums 总时长=%lums 世代=%lu",
       forward ? "快进" : "快退",
       (unsigned long)window.current_ms,
       (unsigned long)s_seek_preview_target_ms,
       (unsigned long)window.total_ms,
       (unsigned long)window.playback_revision);
}

static void player_seek_preview_repeat(bool forward, uint32_t now)
{
  if (s_seek_preview_total_ms == 0) {
    return;
  }

  bool changed = false;
  uint8_t catch_up = 0;
  while (static_cast<int32_t>(now - s_seek_preview_next_repeat_ms) >= 0 &&
         catch_up < 8) {
    const uint32_t next_target = player_seek_step_target(s_seek_preview_target_ms,
                                                        s_seek_preview_total_ms,
                                                        forward);
    if (next_target != s_seek_preview_target_ms) {
      s_seek_preview_target_ms = next_target;
      changed = true;
    }
    s_seek_preview_next_repeat_ms += PLAYER_SEEK_REPEAT_MS;
    ++catch_up;
  }

  if (changed) {
    ui_show_seek_preview(forward ? +1 : -1, s_seek_preview_target_ms);
  }
}

static void player_seek_preview_commit(bool forward)
{
  const uint32_t target_ms = s_seek_preview_target_ms;
  const uint32_t revision = s_seek_preview_revision;
  player_nav_hold_reset(true);

  uint32_t request_id = 0;
  if (!player_seek_to_ms_async(target_ms, revision, &request_id)) {
    ui_show_seek_unavailable();
    s_seek_preview_visible = true;
    s_seek_preview_auto_hide_ms = millis() + 1200;
    LOGW("[按键] %s提交失败：目标=%lums 世代=%lu",
         forward ? "快进" : "快退",
         (unsigned long)target_ms,
         (unsigned long)revision);
    return;
  }

  LOGI("[按键] %s已提交：请求=%lu 目标=%lums",
       forward ? "快进" : "快退",
       (unsigned long)request_id,
       (unsigned long)target_ms);
}

// 返回 true 表示本轮触发了会切换页面的组合动作，keys_update 应立即结束。
static bool handle_player_navigation_keys()
{
  const uint32_t now = millis();
  const int encoder_level = read_key_pin(k_ec06e.pin);
  const int prev_level = read_key_pin(k_prev.pin);
  const int next_level = read_key_pin(k_next.pin);

  const KeyEdge encoder_edge = update_key_edge(k_ec06e, encoder_level, now);
  const KeyEdge prev_edge = update_key_edge(k_prev, prev_level, now);
  const KeyEdge next_edge = update_key_edge(k_next, next_level, now);

  if (s_player_nav_hold_action == PlayerNavHoldAction::None &&
      s_seek_preview_auto_hide_ms != 0 &&
      static_cast<int32_t>(now - s_seek_preview_auto_hide_ms) >= 0) {
    player_nav_hold_reset(true);
  }

  // 快退/快进已经开始后，另一侧导航键只被消费，禁止松开时切歌。
  if (prev_edge == KeyEdge::Pressed &&
      (s_player_nav_hold_action == PlayerNavHoldAction::SeekNext ||
       s_player_nav_hold_action == PlayerNavHoldAction::UnavailableNext)) {
    k_prev.long_fired = true;
  }
  if (next_edge == KeyEdge::Pressed &&
      (s_player_nav_hold_action == PlayerNavHoldAction::SeekPrev ||
       s_player_nav_hold_action == PlayerNavHoldAction::UnavailablePrev)) {
    k_next.long_fired = true;
  }

  if (prev_edge == KeyEdge::Pressed) {
    s_prev_combo_armed = pressed(encoder_level);
    if (s_prev_combo_armed) {
      // 编码器作为组合修饰键，不允许松开时再进入快捷菜单。
      k_ec06e.long_fired = true;
    }
  }

  if (next_edge == KeyEdge::Pressed) {
    s_next_combo_armed = pressed(encoder_level);
    if (s_next_combo_armed) {
      k_ec06e.long_fired = true;
    }
  }

  if (encoder_edge == KeyEdge::Pressed &&
      (pressed(prev_level) || pressed(next_level))) {
    // 编码器必须先按才构成组合键；后按时只消费编码器，原 PREV/NEXT 动作保持不变。
    k_ec06e.long_fired = true;
  }

  // 组合键优先级最高，并要求编码器在 PREV/NEXT 按下前已经处于按下状态。
  if (s_prev_combo_armed && pressed(encoder_level) && pressed(prev_level) &&
      !k_prev.long_fired && now - k_prev.t_down >= PLAYER_COMBO_HOLD_MS) {
    k_prev.long_fired = true;
    k_ec06e.long_fired = true;
    player_nav_hold_reset(true);
    LOGI("[按键] 组合键：编码器 + 长按上一首 -> NFC绑定");
    nfc_bind_popup_open();
    return true;
  }

  if (s_next_combo_armed && pressed(encoder_level) && pressed(next_level) &&
      !k_next.long_fired && now - k_next.t_down >= PLAYER_COMBO_HOLD_MS) {
    k_next.long_fired = true;
    k_ec06e.long_fired = true;
    player_nav_hold_reset(true);
    LOGI("[按键] 组合键：编码器 + 长按下一首 -> 播放列表");
    player_next_group();
    return true;
  }

  // 普通长按 PREV/NEXT：进入快退/快进预览。
  if (s_player_nav_hold_action == PlayerNavHoldAction::None) {
    if (!s_prev_combo_armed && pressed(prev_level) && !k_prev.long_fired &&
        now - k_prev.t_down >= PLAYER_SEEK_HOLD_MS) {
      k_prev.long_fired = true;
      player_seek_preview_begin(false, now);
    } else if (!s_next_combo_armed && pressed(next_level) && !k_next.long_fired &&
               now - k_next.t_down >= PLAYER_SEEK_HOLD_MS) {
      k_next.long_fired = true;
      player_seek_preview_begin(true, now);
    }
  }

  if (s_player_nav_hold_action == PlayerNavHoldAction::SeekPrev && pressed(prev_level)) {
    player_seek_preview_repeat(false, now);
  } else if (s_player_nav_hold_action == PlayerNavHoldAction::SeekNext && pressed(next_level)) {
    player_seek_preview_repeat(true, now);
  }

  if (prev_edge == KeyEdge::Released) {
    const uint32_t held_ms = now - k_prev.t_down;
    if (s_player_nav_hold_action == PlayerNavHoldAction::SeekPrev) {
      player_seek_preview_commit(false);
    } else if (s_player_nav_hold_action == PlayerNavHoldAction::UnavailablePrev) {
      player_nav_hold_reset(true);
    } else if (!s_prev_combo_armed && !k_prev.long_fired && held_ms > 25) {
      player_prev_track();
    }
    s_prev_combo_armed = false;
  }

  if (next_edge == KeyEdge::Released) {
    const uint32_t held_ms = now - k_next.t_down;
    if (s_player_nav_hold_action == PlayerNavHoldAction::SeekNext) {
      player_seek_preview_commit(true);
    } else if (s_player_nav_hold_action == PlayerNavHoldAction::UnavailableNext) {
      player_nav_hold_reset(true);
    } else if (!s_next_combo_armed && !k_next.long_fired && held_ms > 25) {
      player_next_track();
    }
    s_next_combo_armed = false;
  }

  if (encoder_edge == KeyEdge::Released) {
    const uint32_t held_ms = now - k_ec06e.t_down;
    if (!k_ec06e.long_fired && held_ms > 25) {
      enter_quick_menu_from_player();
      return true;
    }
  }

  // 编码器单独长按不执行动作，也不能在松开时误判为短按进入菜单。
  if (pressed(encoder_level) && !k_ec06e.long_fired &&
      now - k_ec06e.t_down >= PLAYER_ENCODER_LONG_MS) {
    k_ec06e.long_fired = true;
  }

  yield();
  return false;
}

void keys_init()
{
  setup_key_pin(PIN_KEY_MODE);
  setup_key_pin(PIN_KEY_PLAY);

  // PLAY 引脚禁用内部下拉，确保外部上拉生效
#if defined(ARDUINO_ARCH_ESP32)
  gpio_pullup_en(static_cast<gpio_num_t>(PIN_KEY_PLAY));
  gpio_pulldown_dis(static_cast<gpio_num_t>(PIN_KEY_PLAY));
#endif

  setup_key_pin(PIN_KEY_PREV);
  setup_key_pin(PIN_KEY_NEXT);
  setup_key_pin(PIN_KEY_MCP_EC06_E);
  setup_key_pin(PIN_KEY_VOLDN);
  setup_key_pin(PIN_KEY_VOLUP);


  // EC06 旋钮初始化
  pinMode(PIN_EC06_A, INPUT_PULLUP);
  pinMode(PIN_EC06_B, INPUT_PULLUP);

  LOGD("[按键] 引脚：模式=%d 播放=%d 上一曲=%d 下一曲=%d 音量减=%d 音量加=%d HALL=%d ec06_a=%d ec06_b=%d",
    PIN_KEY_MODE,
    PIN_KEY_PLAY,
    PIN_KEY_PREV,
    PIN_KEY_NEXT,
    PIN_KEY_VOLDN,
    PIN_KEY_VOLUP,
    PIN_KEY_HALL_OUT,
    PIN_EC06_A,
    PIN_EC06_B);

  // 同步初始电平，避免上电后的误判
  keys_sync_to_hw_state();

  // 初始化旋钮状态
  s_enc_last = read_encoder_state();
  s_enc_accum = 0;
  s_enc_last_step_ms = 0;
}

// 同步当前硬件状态，用于状态切换时避免误判
// 如果按键正按着，就把这次按下直接"消费掉"，防止后续松手时再触发 short
void keys_sync_to_hw_state()
{
  uint32_t now = millis();

  // 页面/状态切换时取消尚未提交的快退快进预览。
  player_nav_hold_reset(true);

  // 同步整组按键前只强制刷新一次 GPIOA。
  (void)refresh_mcp_a_cache(true);

  auto sync_one = [now](KeyCtx& k) {
    k.last = read_key_pin(k.pin);
    k.t_down = now;
    k.t_repeat = now;

    // 如果当前这个键正按着，就把这次按下直接"消费掉"
    // 防止后续松手时再触发 short
    k.long_fired = pressed(k.last);
  };

  sync_one(k_mode);
  sync_one(k_play);
  sync_one(k_prev);
  sync_one(k_next);
  sync_one(k_ec06e);
  sync_one(k_voldn);
  sync_one(k_volup);
}

/*
 * VOLDN 正常态处理：
 * - 双击=切换 WiFi
 * - 按住连发=音量-
 */
static void handle_voldn_key_normal()
{
  uint32_t now = millis();
  int s = read_key_pin(k_voldn.pin);

  // 边沿检测
  if (s != k_voldn.last) {
    k_voldn.last = s;
    if (pressed(s)) {
      k_voldn.t_down = now;
      k_voldn.long_fired = false;
      k_voldn.t_repeat = now;
      ui_volume_key_pressed();
    } else {
      // 松开：检查是否是短按
      if (!k_voldn.long_fired && (now - k_voldn.t_down) > 25 && (now - k_voldn.t_down) < 500) {
        // 短按：进入双击判定窗口
        if (s_voldn_click_pending && now <= s_voldn_click_deadline) {
          // 双击
          voldn_click_commit_double();
        } else {
          // 第一次单击，等待双击
          s_voldn_click_pending = true;
          s_voldn_click_deadline = now + VOLDN_DOUBLE_CLICK_MS;
        }
      }
    }
  }

  // 双击超时：说明是单击（但 VOLDN 单击不做任何事）
  if (s_voldn_click_pending && (int32_t)(now - s_voldn_click_deadline) >= 0) {
    s_voldn_click_pending = false;
    s_voldn_click_deadline = 0;
  }

  // 按住连发（音量）
  // ✅ 渐进式连发：按住时间越长，音量变动越快
  if (pressed(k_voldn.last)) {
    uint32_t hold_time = now - k_voldn.t_down;
    uint32_t repeat_interval = 150; // 默认 150ms 间隔

    // 按住超过 2 秒后加速到 50ms 间隔
    if (hold_time > 2000) {
      repeat_interval = 50;
    }

    if (now - k_voldn.t_repeat >= repeat_interval) {
      k_voldn.t_repeat = now;
      player_volume_step(-5);
    }
  }

  yield();
}

static bool is_any_key_pressed_raw()
{
  return pressed(read_key_pin(k_mode.pin))
      || pressed(read_key_pin(k_play.pin))
      || pressed(read_key_pin(k_prev.pin))
      || pressed(read_key_pin(k_next.pin))
      || pressed(read_key_pin(k_ec06e.pin))
      || pressed(read_key_pin(k_voldn.pin))
      || pressed(read_key_pin(k_volup.pin));
}

static bool handle_backlight_sleep_mode(int8_t encoder_step)
{
  static bool s_sleep_armed = false;

  // 背光开着时，不进入熄屏按键模式。
  if (board_hw_get_backlight()) {
    s_sleep_armed = false;
    return false;
  }

  const bool any_key_pressed = is_any_key_pressed_raw();

  // 刚关闭背光时，关屏的那个按键可能还没松开。
  // 必须等所有按键松开一次，之后才允许熄屏操作。
  if (!s_sleep_armed) {
    if (!any_key_pressed) {
      s_sleep_armed = true;
    }
    return true;
  }

  // 熄屏状态下，旋钮继续调音量，不唤醒屏幕。
  if (encoder_step != 0) {
      player_volume_step(encoder_step > 0 ? ENCODER_VOLUME_STEP : -ENCODER_VOLUME_STEP);
      return true;
  }

  // 熄屏状态下，PLAY 短按仍然播放 / 暂停，长按关机。
  handle_key(k_play,
            play_key_toggle_with_solenoid,
            app_power_save_and_shutdown);

  // 熄屏状态下，PREV / NEXT 短按仍然切歌。
  // 长按类入口需要看屏幕，熄屏下只唤醒。
  handle_key(k_prev,
             player_prev_track,
             [](){
               (void)board_hw_set_backlight(true);
               keys_sync_to_hw_state();
             });

  handle_key(k_next,
             player_next_track,
             [](){
               (void)board_hw_set_backlight(true);
               keys_sync_to_hw_state();
             });

  // MODE / EC06_E 都是界面类操作，熄屏下只负责唤醒屏幕。
  handle_key(k_mode,
             [](){
               (void)board_hw_set_backlight(true);
               keys_sync_to_hw_state();
             },
             [](){
               (void)board_hw_set_backlight(true);
               keys_sync_to_hw_state();
             });

  handle_key(k_ec06e,
             [](){
               (void)board_hw_set_backlight(true);
               keys_sync_to_hw_state();
             },
             [](){
               (void)board_hw_set_backlight(true);
               keys_sync_to_hw_state();
             });

  // 旧板 VOL 键如果存在，也允许熄屏调音量。
  handle_key(k_voldn, nullptr, nullptr, true, [](){ player_volume_step(-5); });
  handle_key(k_volup, nullptr, nullptr, true, [](){ player_volume_step(+5); });

  return true;
}

void keys_update()
{
  // 一轮按键处理共享同一份 MCP GPIOA 快照。总线异常时最多每 100ms 尝试一次，
  // 其余按键按“未按下”处理，避免 I2C 错误风暴。
  (void)refresh_mcp_a_cache(false);

  const int8_t encoder_step = read_encoder_step();

  if (handle_backlight_sleep_mode(encoder_step)) {
    return;
  }

  // --- 列表选择模式 ---
  if (player_list_select_is_active()) {

    // 列表页里旋钮也用于上下移动，不再调音量。
    if (encoder_step > 0) {
      list_select_key_and_refresh(KEY_NEXT_SHORT);
    } else if (encoder_step < 0) {
      list_select_key_and_refresh(KEY_PREV_SHORT);
    }

    // MODE：短按=返回上一级；长按=退出到播放器
    handle_key(k_mode,
               [](){ list_select_key_and_refresh(KEY_MODE_SHORT); },
               [](){ list_select_key_and_refresh(KEY_MODE_LONG); });

    // 编码器按下 / PLAY：短按=确认选择。
    handle_key(k_ec06e,
              [](){ list_select_key_and_refresh(KEY_PLAY_SHORT); },
              nullptr);

    handle_key(k_play,
              [](){ list_select_key_and_refresh(KEY_PLAY_SHORT); },
              nullptr);

    // PREV / NEXT：短按=翻页，编码器旋转负责逐项移动。
    handle_key(k_prev,
              [](){ list_select_key_and_refresh(KEY_PAGE_UP_SHORT); },
              nullptr);

    handle_key(k_next,
              [](){ list_select_key_and_refresh(KEY_PAGE_DOWN_SHORT); },
              nullptr);

    // 旧 VOL 翻页入口已移除，避免残留旧板逻辑影响新交互。
    return;
  }

  // --- 快捷菜单：旋钮导航，按下确认；菜单内不再调整音量 ---
  if (quick_menu_is_active()) {
    quick_menu_tick();

    if (quick_menu_is_active() && encoder_step > 0) {
      quick_menu_key_and_refresh(QuickMenuKey::Down);
    } else if (quick_menu_is_active() && encoder_step < 0) {
      quick_menu_key_and_refresh(QuickMenuKey::Up);
    }

    handle_key(k_ec06e,
               [](){ quick_menu_key_and_refresh(QuickMenuKey::Confirm); },
               [](){ quick_menu_key_and_refresh(QuickMenuKey::Exit); });

    handle_key(k_play,
               [](){ quick_menu_key_and_refresh(QuickMenuKey::Confirm); },
               nullptr);

    handle_key(k_mode,
               [](){ quick_menu_key_and_refresh(QuickMenuKey::Back); },
               [](){ quick_menu_key_and_refresh(QuickMenuKey::Exit); });

    // 快捷菜单中：上一曲/下一曲短按作为翻页键。
    // 普通菜单页里等价于上一项/下一项；NFC列表页里是真正上一页/下一页。
    handle_key(k_prev,
              [](){ quick_menu_key_and_refresh(QuickMenuKey::PageUp); },
              nullptr);

    handle_key(k_next,
              [](){ quick_menu_key_and_refresh(QuickMenuKey::PageDown); },
              nullptr);

    return;
  }

  // --- NFC 管理状态下，按键转给 admin 状态机处理 ---
  if (g_app_state == STATE_NFC_ADMIN) {
    handle_key(k_mode, [](){ nfc_admin_state_on_key(NFC_ADMIN_KEY_MODE_SHORT); }, nullptr);

    // 绑定刷卡后的确认页：PLAY 和编码器按下都作为“确认绑定”。
    handle_key(k_play, [](){ nfc_admin_state_on_key(NFC_ADMIN_KEY_PLAY_SHORT); }, nullptr);
    handle_key(k_ec06e, [](){ nfc_admin_state_on_key(NFC_ADMIN_KEY_PLAY_SHORT); }, nullptr);
    return;
  }

  // --- 扫描状态下的紧急处理 ---
  const AppRescanState rescan = app_rescan_state_get();
  if (rescan.rescanning) {
    // 扫描时只允许 MODE 取消，但必须用“按下沿”而不是电平。
    // 否则由 MODE 长按启动重扫后，会因为按键仍保持按下而立刻触发取消。
    int s = read_key_pin(k_mode.pin);

    if (!s_rescan_cancel_armed) {
      // 先等待启动重扫的这次长按释放，再允许取消。
      if (!pressed(s)) {
        s_rescan_cancel_armed = true;
      }
      k_mode.last = s;
      return;
    }

    if (s != k_mode.last) {
      k_mode.last = s;
      if (pressed(s) && !rescan.abort_requested) {
        if (app_request_cancel_rescan()) {
          LOGI("[按键] 已发送曲库重扫取消信号");
        }
      }
    }

    return;
  }

  // --- 正常播放模式 ---

  // NFC 绑定类型弹窗优先处理。
  // 弹窗打开时，旋钮/确认/取消不再透传给播放器，避免误切歌或误调音量。
  if (nfc_bind_popup_handle_if_active(encoder_step)) {
    return;
  }

  // 正常播放页持续刷新 X5 超时状态。
  // 否则 X5 超时后如果用户没有再转动旋钮，内部 s_volume_fast_mode 仍可能保持 true，
  // 下一次短按 MODE 会被误判为“从 X5 切回 X1”。
  (void)volume_fast_mode_is_active();

  // 正常播放页：旋钮控制音量。
  handle_encoder_volume_step(encoder_step);

  // EC06_E / PREV / NEXT 共用组合键状态机：
  // - EC06_E 短按：快捷菜单
  // - PREV/NEXT 短按：上一首/下一首
  // - PREV/NEXT 长按：快退/快进，松手只提交一次 seek
  // - 编码器先按住 + PREV/NEXT 长按：NFC绑定/播放列表
  if (handle_player_navigation_keys()) {
    return;
  }

  // MODE：短按切换大步音量模式；长按切换播放界面视图。
  handle_key(k_mode, volume_fast_mode_toggle, ui_toggle_view);

  // PLAY：短按输出一次电磁铁短脉冲 + 播放/暂停，长按保存 NVS 后关机。
  handle_key(k_play, play_key_toggle_with_solenoid, app_power_save_and_shutdown);

  // VOL：旧板音量按键逻辑。新 PCB1 已禁用，不影响。
  handle_voldn_key_normal();
  handle_key(k_volup, nullptr, nullptr, true, [](){ player_volume_step(+5); });
}
