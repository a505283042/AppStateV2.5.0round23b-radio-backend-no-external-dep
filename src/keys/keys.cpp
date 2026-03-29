#include <Arduino.h>
#include "keys/keys.h"
#include "keys/keys_pins.h"

#include "app_flags.h"
#include "app_state.h"
#include "ui/ui.h"
#include "player_control.h"
#include "player_list_select.h"
#include "nfc/nfc_admin_state.h"
#include "utils/log.h"

/*
 * 按键输入模块。
 *
 * 当前 MODE 键的语义：
 * - 单击：切换小类（顺序 / 随机）
 * - 双击：切换大类（全部 / 歌手 / 专辑）
 * - 长按：触发重扫
 *
 * 说明：为了支持双击识别，MODE 单击会有一个短暂等待窗口。
 */

static inline bool pressed(int level) { return level == LOW; } // 按下接地

struct KeyCtx {
  uint8_t pin;
  int last;
  uint32_t t_down;
  bool long_fired;
  uint32_t t_repeat;
};

static KeyCtx k_mode  { PIN_KEY_MODE,  HIGH, 0, false, 0 };
static KeyCtx k_play  { PIN_KEY_PLAY,  HIGH, 0, false, 0 };
static KeyCtx k_prev  { PIN_KEY_PREV,  HIGH, 0, false, 0 };
static KeyCtx k_next  { PIN_KEY_NEXT,  HIGH, 0, false, 0 };
static KeyCtx k_voldn { PIN_KEY_VOLDN, HIGH, 0, false, 0 };
static KeyCtx k_volup { PIN_KEY_VOLUP, HIGH, 0, false, 0 };

static bool s_rescan_cancel_armed = false;

static bool s_mode_click_pending = false;
static uint32_t s_mode_click_deadline = 0;
static constexpr uint32_t MODE_DOUBLE_CLICK_MS = 320;

static void mode_click_reset()
{
  s_mode_click_pending = false;
  s_mode_click_deadline = 0;
}

/* MODE 单击提交：仅切小类，不改变大类。 */
static void mode_click_commit_single()
{
  ui_mode_switch_highlight();
  player_toggle_random();
  mode_click_reset();
}

/* MODE 双击提交：仅切大类，保留当前顺序/随机状态。 */
static void mode_click_commit_double()
{
  ui_mode_switch_highlight();
  player_cycle_mode_category();
  mode_click_reset();
}

/* 由 MODE 长按触发重扫；会先记录当前歌曲路径用于后续恢复。 */
static void start_rescan()
{
  if (app_request_start_rescan()) {
    // 由 MODE 长按进入重扫时，当前 MODE 仍可能保持按下。
    // 扫描态的取消逻辑必须等待这次长按先松开，再接受下一次按下沿。
    s_rescan_cancel_armed = false;
  }
}

static void handle_key(KeyCtx& k,
                       void (*on_short)(),
                       void (*on_long)(),
                       bool repeat = false,
                       void (*on_repeat)() = nullptr)
{
  uint32_t now = millis();
  int s = digitalRead(k.pin);

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

void keys_init()
{
  pinMode(PIN_KEY_MODE,  INPUT_PULLUP);
  pinMode(PIN_KEY_PLAY,  INPUT_PULLUP);
  pinMode(PIN_KEY_PREV,  INPUT_PULLUP);
  pinMode(PIN_KEY_NEXT,  INPUT_PULLUP);
  pinMode(PIN_KEY_VOLDN, INPUT_PULLUP);
  pinMode(PIN_KEY_VOLUP, INPUT_PULLUP);

  // 同步初始电平，避免上电后的误判
  keys_sync_to_hw_state();
}

// 同步当前硬件状态，用于状态切换时避免误判
// 如果按键正按着，就把这次按下直接"消费掉"，防止后续松手时再触发 short
void keys_sync_to_hw_state()
{
  uint32_t now = millis();

  auto sync_one = [now](KeyCtx& k) {
    k.last = digitalRead(k.pin);
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
  sync_one(k_voldn);
  sync_one(k_volup);

  mode_click_reset();
}

/*
 * MODE 正常态处理：
 * - 短按进入单击/双击判定窗口
 * - 长按直接触发重扫
 */
static void handle_mode_key_normal()
{
  uint32_t now = millis();
  int s = digitalRead(k_mode.pin);

  if (s != k_mode.last) {
    k_mode.last = s;
    if (pressed(s)) {
      k_mode.t_down = now;
      k_mode.long_fired = false;
      k_mode.t_repeat = now;
    } else {
      if (!k_mode.long_fired && (now - k_mode.t_down) > 25) {
        if (s_mode_click_pending && now <= s_mode_click_deadline) {
          mode_click_commit_double();
        } else {
          s_mode_click_pending = true;
          s_mode_click_deadline = now + MODE_DOUBLE_CLICK_MS;
        }
      }
    }
  }

  if (pressed(k_mode.last) && !k_mode.long_fired && (now - k_mode.t_down) >= 800) {
    k_mode.long_fired = true;
    mode_click_reset();
    start_rescan();
  }

  if (s_mode_click_pending && (int32_t)(now - s_mode_click_deadline) >= 0) {
    mode_click_commit_single();
  }

  yield();
}

void keys_update()
{
  // --- NFC 管理状态下，按键转给 admin 状态机处理 ---
  if (g_app_state == STATE_NFC_ADMIN) {
    mode_click_reset();
    handle_key(k_mode, [](){ nfc_admin_state_on_key(NFC_ADMIN_KEY_MODE_SHORT); }, nullptr);
    handle_key(k_play, [](){ nfc_admin_state_on_key(NFC_ADMIN_KEY_PLAY_SHORT); }, nullptr);
    return;
  }

  // --- 新增：扫描状态下的紧急处理 ---
  if (g_rescanning) {
    mode_click_reset();
    // 扫描时只允许 MODE 取消，但必须用“按下沿”而不是电平。
    // 否则由 MODE 长按启动重扫后，会因为按键仍保持按下而立刻触发取消。
    int s = digitalRead(k_mode.pin);

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
      if (pressed(s) && !g_abort_scan) {
        g_abort_scan = true;
        LOGI("[KEYS] Abort signal sent!");
      }
    }
    return; // 扫描时屏蔽其他按键逻辑
  }

  // 检查是否处于列表选择模式
  if (player_list_select_is_active()) {
    mode_click_reset();
    // 列表选择模式下的按键处理
    // MODE：短按=返回；长按=取消选择
    handle_key(k_mode,  [](){ player_list_select_handle_key(KEY_MODE_SHORT); }, [](){ player_list_select_handle_key(KEY_MODE_LONG); });

    // PLAY：短按=确认选择
    handle_key(k_play,  [](){ player_list_select_handle_key(KEY_PLAY_SHORT); }, nullptr);

    // PREV / NEXT：短按=上下移动选择
    handle_key(k_prev,  [](){ player_list_select_handle_key(KEY_PREV_SHORT); }, nullptr);
    handle_key(k_next,  [](){ player_list_select_handle_key(KEY_NEXT_SHORT); }, nullptr);

    // VOL：短按=快速翻页
    handle_key(k_voldn, [](){ player_list_select_handle_key(KEY_VOLDN_SHORT); }, nullptr);
    handle_key(k_volup, [](){ player_list_select_handle_key(KEY_VOLUP_SHORT); }, nullptr);
    return;
  }

  // 正常播放模式
  // MODE：单击=切小类（顺序/随机）；双击=切大类（全部/歌手/专辑）；长按=重扫
  handle_mode_key_normal();

  // PLAY：短按=播放/停止；长按=切换视图
  handle_key(k_play,  player_toggle_play, ui_toggle_view);

  // PREV / NEXT：短按=切歌，长按 PREV=进入NFC管理，长按 NEXT=进入列表选择模式
  handle_key(k_prev,  player_prev_track, [](){ (void)app_request_enter_nfc_admin(); });
  handle_key(k_next,  player_next_track, player_next_group);

  // VOL：按住连发
  handle_key(k_voldn, nullptr, nullptr, true, [](){ player_volume_step(-5); });
  handle_key(k_volup, nullptr, nullptr, true, [](){ player_volume_step(+5); });
}