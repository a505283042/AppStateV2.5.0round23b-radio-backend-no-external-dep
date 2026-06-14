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
#include "board/board_pins_pcb1_mcp23017.h"
#include "hal/mcp23017_u3.h"
#include "hal/board_hw_control.h"

// 播放器页 NFC 小弹窗直接复用快捷菜单里已经写好的绑定入口。
// 这里再声明一次，避免不同编译单元包含顺序造成未声明报错。
bool quick_menu_nfc_bind_current_track();
bool quick_menu_nfc_bind_current_artist();
bool quick_menu_nfc_bind_current_album();

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

// HALL_OUT：GPIO9 霍尔输入，默认按低电平触发一次播放/暂停。
// 如果后续实测模块为高电平触发，只需要把 HALL_ACTIVE_LEVEL 改为 HIGH。
static constexpr int HALL_ACTIVE_LEVEL = LOW;
static constexpr uint32_t HALL_DEBOUNCE_MS = 60;
static constexpr uint32_t HALL_TRIGGER_GUARD_MS = 800;

static int s_hall_last_raw = HIGH;
static int s_hall_stable = HIGH;
static uint32_t s_hall_raw_change_ms = 0;
static uint32_t s_hall_last_trigger_ms = 0;

// VOLDN 双击检测
static bool s_voldn_click_pending = false;
static uint32_t s_voldn_click_deadline = 0;
static constexpr uint32_t VOLDN_DOUBLE_CLICK_MS = 320;

static bool s_rescan_cancel_armed = false;

static bool s_mode_click_pending = false;
static uint32_t s_mode_click_deadline = 0;
static constexpr uint32_t MODE_DOUBLE_CLICK_MS = 320;

// EC06 旋钮相关
static int s_enc_last = 0;
static int s_enc_accum = 0;
static uint32_t s_enc_last_step_ms = 0;

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
    // g_play_mode 的 6 个值已经在 ui.h 里定义，这里直接按枚举判断，
    // 不依赖 player_playlist.cpp 里的辅助函数，减少按键模块的耦合。
    switch (g_play_mode) {
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

    // 长按 PREV 触发弹窗后，消费当前按键电平，避免松手又触发短按上一曲。
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

static void enter_quick_menu_from_player()
{
    volume_fast_mode_exit();
    quick_menu_enter();

    // 进入快捷菜单后立即同步一次按键状态。
    // 这样本轮按键扫描后半段不会继续按“播放器页”语义处理 PREV/NEXT，
    // 可避免开机第一次进菜单时残留按键边沿被误当成切歌。
    keys_sync_to_hw_state();
}

static int read_mcp_a_active_low(uint8_t bit)
{
    if (!mcp23017_u3_is_ready()) {
        return HIGH;
    }

    const uint8_t a = mcp23017_u3_read_a();
    return (a & (1 << bit)) ? HIGH : LOW;
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

static int read_hall_out_level()
{
#if defined(PIN_KEY_HALL_OUT)
    if (PIN_KEY_HALL_OUT < 0) {
        return HIGH;
    }
    return digitalRead(PIN_KEY_HALL_OUT);
#else
    return HIGH;
#endif
}

static void hall_out_sync_to_hw_state()
{
    const int level = read_hall_out_level();
    const uint32_t now = millis();

    s_hall_last_raw = level;
    s_hall_stable = level;
    s_hall_raw_change_ms = now;
    s_hall_last_trigger_ms = now;
}

static void handle_hall_out_play_pause()
{
#if defined(PIN_KEY_HALL_OUT)
    if (PIN_KEY_HALL_OUT < 0) {
        return;
    }

    // 扫描 / NFC 绑定确认页不处理霍尔输入，避免管理流程中误触发播放状态。
    if (g_rescanning || g_app_state == STATE_NFC_ADMIN) {
        return;
    }

    const uint32_t now = millis();
    const int raw = read_hall_out_level();

    if (raw != s_hall_last_raw) {
        s_hall_last_raw = raw;
        s_hall_raw_change_ms = now;
    }

    if (raw == s_hall_stable || (now - s_hall_raw_change_ms) < HALL_DEBOUNCE_MS) {
        return;
    }

    const int old_stable = s_hall_stable;
    s_hall_stable = raw;

    // 只在进入有效电平时触发一次，释放时不触发。
    if (old_stable != HALL_ACTIVE_LEVEL && s_hall_stable == HALL_ACTIVE_LEVEL) {
        if (now - s_hall_last_trigger_ms < HALL_TRIGGER_GUARD_MS) {
            return;
        }

        s_hall_last_trigger_ms = now;
        LOGI("[HALL] GPIO%d 触发播放/暂停", PIN_KEY_HALL_OUT);
        player_toggle_play();
    }
#else
    (void)player_toggle_play;
#endif
}

/* VOLDN 双击提交：切换 WiFi */
static void voldn_click_commit_double()
{
  web_wifi_toggle();
  LOGW("[应用] WiFi 已切换：%s", web_wifi_is_enabled() ? "开启" : "关闭");
  s_voldn_click_pending = false;
  s_voldn_click_deadline = 0;
}

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

#if defined(PIN_KEY_HALL_OUT)
  setup_key_pin(PIN_KEY_HALL_OUT);
#endif

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
  hall_out_sync_to_hw_state();

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
  hall_out_sync_to_hw_state();

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
  int s = read_key_pin(k_mode.pin);

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
            player_toggle_play,
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
  const int8_t encoder_step = read_encoder_step();

  // HALL_OUT 是独立传感器输入，优先处理。
  // 即使背光熄灭，也允许磁吸触发播放/暂停。
  handle_hall_out_play_pause();

  if (handle_backlight_sleep_mode(encoder_step)) {
    return;
  }

  // --- 列表选择模式 ---
  if (player_list_select_is_active()) {
    mode_click_reset();

    // 列表页里旋钮也用于上下移动，不再调音量。
    if (encoder_step > 0) {
      player_list_select_handle_key(KEY_NEXT_SHORT);
    } else if (encoder_step < 0) {
      player_list_select_handle_key(KEY_PREV_SHORT);
    }

    // MODE：短按=返回上一级；长按=退出到播放器
    handle_key(k_mode,
               [](){ player_list_select_handle_key(KEY_MODE_SHORT); },
               [](){ player_list_select_handle_key(KEY_MODE_LONG); });

    // 编码器按下 / PLAY：短按=确认选择。
    handle_key(k_ec06e,
              [](){ player_list_select_handle_key(KEY_PLAY_SHORT); },
              nullptr);

    handle_key(k_play,
              [](){ player_list_select_handle_key(KEY_PLAY_SHORT); },
              nullptr);

    // PREV / NEXT：短按=翻页，编码器旋转负责逐项移动。
    handle_key(k_prev,
              [](){ player_list_select_handle_key(KEY_PAGE_UP_SHORT); },
              nullptr);

    handle_key(k_next,
              [](){ player_list_select_handle_key(KEY_PAGE_DOWN_SHORT); },
              nullptr);

    // 旧 VOL 翻页入口已移除，避免残留旧板逻辑影响新交互。
    return;
  }

  // --- 快捷菜单：旋钮导航，按下确认；菜单内不再调整音量 ---
  if (quick_menu_is_active()) {
    mode_click_reset();
    quick_menu_tick();

    if (quick_menu_is_active() && encoder_step > 0) {
      quick_menu_handle_key(QuickMenuKey::Down);
    } else if (quick_menu_is_active() && encoder_step < 0) {
      quick_menu_handle_key(QuickMenuKey::Up);
    }

    handle_key(k_ec06e,
               [](){ quick_menu_handle_key(QuickMenuKey::Confirm); },
               [](){ quick_menu_handle_key(QuickMenuKey::Exit); });

    handle_key(k_play,
               [](){ quick_menu_handle_key(QuickMenuKey::Confirm); },
               nullptr);

    handle_key(k_mode,
               [](){ quick_menu_handle_key(QuickMenuKey::Back); },
               [](){ quick_menu_handle_key(QuickMenuKey::Exit); });

    // 快捷菜单中：上一曲/下一曲短按作为翻页键。
    // 普通菜单页里等价于上一项/下一项；NFC列表页里是真正上一页/下一页。
    handle_key(k_prev,
              [](){ quick_menu_handle_key(QuickMenuKey::PageUp); },
              nullptr);

    handle_key(k_next,
              [](){ quick_menu_handle_key(QuickMenuKey::PageDown); },
              nullptr);

    return;
  }

  // --- NFC 管理状态下，按键转给 admin 状态机处理 ---
  if (g_app_state == STATE_NFC_ADMIN) {
    mode_click_reset();
    handle_key(k_mode, [](){ nfc_admin_state_on_key(NFC_ADMIN_KEY_MODE_SHORT); }, nullptr);

    // 绑定刷卡后的确认页：PLAY 和编码器按下都作为“确认绑定”。
    handle_key(k_play, [](){ nfc_admin_state_on_key(NFC_ADMIN_KEY_PLAY_SHORT); }, nullptr);
    handle_key(k_ec06e, [](){ nfc_admin_state_on_key(NFC_ADMIN_KEY_PLAY_SHORT); }, nullptr);
    return;
  }

  // --- 扫描状态下的紧急处理 ---
  if (g_rescanning) {
    mode_click_reset();

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
      if (pressed(s) && !g_abort_scan) {
        g_abort_scan = true;
        LOGI("[按键] 已发送中止信号");
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

  // EC06_E：短按进入快捷菜单。
  handle_key(k_ec06e, enter_quick_menu_from_player, nullptr);

  // MODE：短按切换大步音量模式。
  mode_click_reset();
  handle_key(k_mode, volume_fast_mode_toggle, nullptr);

  // PLAY：短按播放/暂停，长按保存 NVS 后关机。
  handle_key(k_play, player_toggle_play, app_power_save_and_shutdown);

  // PREV / NEXT：短按=切歌，长按 PREV=弹出 NFC 绑定类型选择，长按 NEXT=进入列表选择模式。
  handle_key(k_prev, player_prev_track, nfc_bind_popup_open);
  handle_key(k_next, player_next_track, player_next_group);

  // VOL：旧板音量按键逻辑。新 PCB1 已禁用，不影响。
  handle_voldn_key_normal();
  handle_key(k_volup, nullptr, nullptr, true, [](){ player_volume_step(+5); });
}