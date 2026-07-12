#include "audio/audio_output_route.h"

#include <Preferences.h>

#include "audio/audio.h"
#include "audio/audio_service.h"
#include "hal/bt62sp_uart_debug.h"
#include "hal/board_hw_control.h"
#include "ui/ui.h"
#include "utils/log.h"

namespace {

// 默认使用功放输出；3.5 耳机/Line out 始终常通，不受本状态控制。
volatile uint8_t s_route = static_cast<uint8_t>(AudioOutputRoute::Speaker);

// 蓝牙发射模式下，BT62SP 的 L/R 是模拟输入。播放器音量过高会让模块输入级过载，
// 蓝牙音箱端听起来会发糊。因此进入蓝牙发射后固定播放器音量，只调 BT62SP AT+VOL。
static constexpr uint8_t BT_TX_PLAYER_FIXED_VOLUME = 20;
// BT62SP 上电默认音量为 50%，模块不会自动持久化 AT+VOL 设置。
// ESP32 使用 NVS 保存最近一次蓝牙发射音量；首次没有保存值时使用 50%。
static constexpr uint8_t BT_TX_VOLUME_DEFAULT = 50;
static constexpr char BT_TX_PREFS_NAMESPACE[] = "audio_route";
static constexpr char BT_TX_PREFS_KEY[] = "bt_vol";

// 模块上电后可能先接收命令、稍后才完成音频运行态初始化。
// 先在固定等待后应用一次；收到 CLEAR OK 后再应用一次最新值。
static constexpr uint32_t BT_TX_VOLUME_APPLY_FALLBACK_MS = 2500;
static constexpr uint32_t BT_TX_VOLUME_APPLY_AFTER_READY_MS = 300;
static constexpr uint32_t BT_TX_VOLUME_APPLY_RETRY_MS = 100;

volatile uint8_t s_bt_tx_volume = BT_TX_VOLUME_DEFAULT;
volatile uint8_t s_saved_normal_volume = 80;
volatile bool s_bt_tx_volume_known = false;
bool s_bt_tx_volume_policy_active = false;
bool s_bt_tx_nvs_loaded = false;
bool s_bt_tx_nvs_valid = false;
uint8_t s_bt_tx_persisted_volume = BT_TX_VOLUME_DEFAULT;
bool s_bt_tx_apply_pending = false;
uint32_t s_bt_tx_apply_due_ms = 0;
uint32_t s_bt_tx_ready_generation_seen = 0;

AudioOutputRoute current_route()
{
    return static_cast<AudioOutputRoute>(s_route);
}

bool set_route(AudioOutputRoute route)
{
    // 路由状态和相关硬件切换统一交给 AudioTask 串行执行。
    return audio_service_set_output_route(route, true);
}

bool route_allows_amp()
{
    return current_route() == AudioOutputRoute::Speaker;
}

bool time_reached(uint32_t now, uint32_t target)
{
    return static_cast<int32_t>(now - target) >= 0;
}

void load_bt_tx_volume_from_nvs_once()
{
    if (s_bt_tx_nvs_loaded) {
        return;
    }

    s_bt_tx_nvs_loaded = true;
    s_bt_tx_nvs_valid = false;
    s_bt_tx_persisted_volume = BT_TX_VOLUME_DEFAULT;
    s_bt_tx_volume = BT_TX_VOLUME_DEFAULT;

    Preferences pref;
    if (!pref.begin(BT_TX_PREFS_NAMESPACE, true)) {
        LOGW("[音量] 打开蓝牙音量 NVS 失败，首次使用默认值=%u",
             (unsigned)BT_TX_VOLUME_DEFAULT);
        s_bt_tx_volume_known = true;
        return;
    }

    if (pref.isKey(BT_TX_PREFS_KEY)) {
        const uint8_t saved = pref.getUChar(BT_TX_PREFS_KEY, BT_TX_VOLUME_DEFAULT);
        if (saved <= 100) {
            s_bt_tx_volume = saved;
            s_bt_tx_persisted_volume = saved;
            s_bt_tx_nvs_valid = true;
            LOGI("[音量] 已从 NVS 读取蓝牙发射音量=%u", (unsigned)saved);
        } else {
            LOGW("[音量] NVS 蓝牙音量无效=%u，改用默认值=%u",
                 (unsigned)saved,
                 (unsigned)BT_TX_VOLUME_DEFAULT);
        }
    } else {
        LOGI("[音量] NVS 尚无蓝牙音量，首次使用默认值=%u",
             (unsigned)BT_TX_VOLUME_DEFAULT);
    }

    pref.end();
    s_bt_tx_volume_known = true;
}

bool save_bt_tx_volume_to_nvs_if_needed()
{
    load_bt_tx_volume_from_nvs_once();

    const uint8_t volume = s_bt_tx_volume > 100 ? 100 : s_bt_tx_volume;
    if (s_bt_tx_nvs_valid && s_bt_tx_persisted_volume == volume) {
        LOGD("[音量] 蓝牙发射音量未变化，跳过 NVS 写入=%u", (unsigned)volume);
        return true;
    }

    Preferences pref;
    if (!pref.begin(BT_TX_PREFS_NAMESPACE, false)) {
        LOGE("[音量] 保存蓝牙发射音量失败：打开 NVS namespace");
        return false;
    }

    const bool ok = pref.putUChar(BT_TX_PREFS_KEY, volume) > 0;
    pref.end();

    if (!ok) {
        LOGE("[音量] 保存蓝牙发射音量失败：写入 NVS");
        return false;
    }

    s_bt_tx_persisted_volume = volume;
    s_bt_tx_nvs_valid = true;
    LOGI("[音量] 退出蓝牙发射，已保存音量到 NVS=%u", (unsigned)volume);
    return true;
}

void schedule_bt_tx_volume_apply(uint32_t delay_ms)
{
    s_bt_tx_apply_pending = true;
    s_bt_tx_apply_due_ms = millis() + delay_ms;
}

void sync_ui_volume_from_route_state()
{
    if (current_route() == AudioOutputRoute::BluetoothTx && !s_bt_tx_volume_known) {
        ui_set_volume_unknown();
        return;
    }

    ui_set_volume(current_route() == AudioOutputRoute::BluetoothTx
                      ? s_bt_tx_volume
                      : audio_get_volume());
}

void apply_bluetooth_tx_volume_policy()
{
    if (!s_bt_tx_volume_policy_active) {
        s_saved_normal_volume = audio_get_volume();
        s_bt_tx_volume_policy_active = true;
        LOGD("[音量] 进入蓝牙发射：保存播放器音量=%u", (unsigned)s_saved_normal_volume);
    }

    load_bt_tx_volume_from_nvs_once();

    // 送入 BT62SP 模拟输入的播放器音量固定在安全值。
    // BT62SP 不负责持久化音量；进入发射模式时由 ESP32 把 NVS 保存值主动下发。
    audio_set_volume(BT_TX_PLAYER_FIXED_VOLUME);
    bt62sp_uart_debug_cancel_volume_query();
    s_bt_tx_ready_generation_seen = bt62sp_uart_debug_ready_generation();
    schedule_bt_tx_volume_apply(BT_TX_VOLUME_APPLY_FALLBACK_MS);
    sync_ui_volume_from_route_state();

    LOGI("[音量] 蓝牙发射：播放器固定=%u，等待下发 NVS 音量=%u",
         (unsigned)BT_TX_PLAYER_FIXED_VOLUME,
         (unsigned)s_bt_tx_volume);
}

void restore_normal_volume_policy_if_needed()
{
    if (!s_bt_tx_volume_policy_active) {
        return;
    }

    // 在关闭 BT62SP 电源前保存当前音量。只有数值变化时才写 NVS，避免无效擦写。
    (void)save_bt_tx_volume_to_nvs_if_needed();

    bt62sp_uart_debug_cancel_volume_query();
    s_bt_tx_apply_pending = false;
    s_bt_tx_apply_due_ms = 0;
    s_bt_tx_ready_generation_seen = bt62sp_uart_debug_ready_generation();

    const uint8_t restore = s_saved_normal_volume > 100 ? 100 : s_saved_normal_volume;
    s_bt_tx_volume_policy_active = false;
    audio_set_volume(restore);
    ui_set_volume(restore);
    LOGD("[音量] 离开蓝牙发射：恢复播放器音量=%u", (unsigned)restore);
}

} // namespace

AudioOutputRoute audio_output_route_get()
{
    return current_route();
}

const char* audio_output_route_label()
{
    if (audio_output_route_is_headphone_only()) {
        return "仅耳机";
    }

    if (audio_output_route_is_bluetooth_tx()) {
        return "耳机+蓝牙";
    }

    return "耳机+功放";
}

bool audio_output_route_is_headphone_only()
{
    return current_route() == AudioOutputRoute::HeadphoneOnly;
}

bool audio_output_route_is_speaker()
{
    return current_route() == AudioOutputRoute::Speaker;
}

bool audio_output_route_is_bluetooth_tx()
{
    return current_route() == AudioOutputRoute::BluetoothTx;
}

uint8_t audio_output_route_bluetooth_tx_player_fixed_volume()
{
    return BT_TX_PLAYER_FIXED_VOLUME;
}

uint8_t audio_output_route_bluetooth_tx_volume()
{
    return s_bt_tx_volume;
}

bool audio_output_route_bluetooth_tx_volume_known()
{
    return s_bt_tx_volume_known;
}

uint8_t audio_output_route_get_user_volume()
{
    if (audio_output_route_is_bluetooth_tx()) {
        // 查询成功前没有可信的 BT62SP 绝对音量；返回正常路线保存值只用于快照/兼容读取，
        // UI 会单独显示“--”，相对调节也不会使用这个回退值。
        return s_bt_tx_volume_known ? s_bt_tx_volume : s_saved_normal_volume;
    }
    return audio_get_volume();
}

bool audio_output_route_set_user_volume(uint8_t value)
{
    return audio_service_set_user_volume(value, true);
}

bool audio_output_route_step_user_volume(int delta)
{
    return audio_service_step_user_volume(delta, true);
}

void audio_output_route_sync_ui_volume()
{
    sync_ui_volume_from_route_state();
}

bool audio_output_route_select_headphone_only()
{
    return set_route(AudioOutputRoute::HeadphoneOnly);
}

bool audio_output_route_select_speaker()
{
    return set_route(AudioOutputRoute::Speaker);
}

bool audio_output_route_select_bluetooth_tx()
{
    return set_route(AudioOutputRoute::BluetoothTx);
}

bool audio_output_route_enforce()
{
    return audio_service_set_output_route(current_route(), true);
}

bool audio_output_route_set_amp_mute(bool enabled)
{
    return audio_service_set_amp_mute(enabled, true);
}

bool audio_output_route_set_amp_shutdown(bool enabled)
{
    return audio_service_set_amp_shutdown(enabled, true);
}

bool audio_output_route_apply_from_audio_task(AudioOutputRoute route)
{
    // 只有 AudioTask 可以更新路由并直接操作音频输出硬件。
    s_route = static_cast<uint8_t>(route);

    bool ok = true;

    if (audio_output_route_is_bluetooth_tx()) {
        // 耳机+蓝牙：功放必须保持关闭，避免喇叭与蓝牙同时出声。
        ok = board_hw_set_amp_mute(true) && ok;
        ok = board_hw_set_amp_shutdown(true) && ok;
        ok = board_hw_set_bt_mode(true) && ok;
        ok = board_hw_set_bt_power(true) && ok;
        apply_bluetooth_tx_volume_policy();
        LOGD("[音频输出] 路线=耳机+蓝牙，功放静音并关断，蓝牙切到发射模式并上电");
        return ok;
    }

    if (audio_output_route_is_headphone_only()) {
        restore_normal_volume_policy_if_needed();
        // 仅耳机：关闭蓝牙，同时保持功放静音和关断。
        ok = board_hw_set_bt_power(false) && ok;
        ok = board_hw_set_bt_mode(false) && ok;
        ok = board_hw_set_amp_mute(true) && ok;
        ok = board_hw_set_amp_shutdown(true) && ok;
        LOGD("[音频输出] 路线=仅耳机，功放静音并关断，蓝牙关闭并回到接收模式");
        return ok;
    }

    restore_normal_volume_policy_if_needed();
    // 切到功放时先解除关断但继续静音。是否取消静音由 AudioTask 根据播放/暂停状态决定。
    ok = board_hw_set_bt_power(false) && ok;
    ok = board_hw_set_bt_mode(false) && ok;
    ok = board_hw_set_amp_mute(true) && ok;
    ok = board_hw_set_amp_shutdown(false) && ok;
    LOGD("[音频输出] 路线=耳机+功放，功放已解除关断并保持静音");
    return ok;
}

bool audio_output_route_set_amp_mute_from_audio_task(bool enabled)
{
    if (!enabled && !route_allows_amp()) {
        // 非功放模式下，任何取消静音请求都改为保持静音。
        LOGD("[音频输出] 功放取消静音被阻止：路线=%s", audio_output_route_label());
        return board_hw_set_amp_mute(true);
    }

    return board_hw_set_amp_mute(enabled);
}

bool audio_output_route_set_amp_shutdown_from_audio_task(bool enabled)
{
    if (!enabled && !route_allows_amp()) {
        // 非功放模式下，任何释放关断请求都改为继续关断。
        LOGD("[音频输出] 功放解除关断被阻止：路线=%s", audio_output_route_label());
        bool ok = board_hw_set_amp_mute(true);
        ok = board_hw_set_amp_shutdown(true) && ok;
        return ok;
    }

    return board_hw_set_amp_shutdown(enabled);
}


void audio_output_route_poll_from_audio_task()
{
    if (!audio_output_route_is_bluetooth_tx()) {
        return;
    }

    const uint32_t now = millis();
    const uint32_t ready_generation = bt62sp_uart_debug_ready_generation();

    if (ready_generation != 0 && ready_generation != s_bt_tx_ready_generation_seen) {
        s_bt_tx_ready_generation_seen = ready_generation;

        // CLEAR OK 表示模块音频运行态已经重新初始化。
        // 再次下发当前 NVS 音量，避免上电早期命令被模块忽略。
        schedule_bt_tx_volume_apply(BT_TX_VOLUME_APPLY_AFTER_READY_MS);
        LOGI("[音量] BT62SP 启动完成，准备重新应用 NVS 音量：代次=%lu 音量=%u",
             (unsigned long)ready_generation,
             (unsigned)s_bt_tx_volume);
    }

    if (!s_bt_tx_apply_pending || !time_reached(now, s_bt_tx_apply_due_ms)) {
        return;
    }

    if (bt62sp_uart_debug_set_volume(s_bt_tx_volume)) {
        s_bt_tx_apply_pending = false;
        s_bt_tx_apply_due_ms = 0;
        sync_ui_volume_from_route_state();
        LOGI("[音量] 已向 BT62SP 应用 NVS 音量=%u", (unsigned)s_bt_tx_volume);
    } else {
        // UART 尚未就绪时短暂重试，保持非阻塞。
        s_bt_tx_apply_due_ms = now + BT_TX_VOLUME_APPLY_RETRY_MS;
    }
}

bool audio_output_route_set_user_volume_from_audio_task(uint8_t value)
{
    if (value > 100) {
        value = 100;
    }

    if (audio_output_route_is_bluetooth_tx()) {
        load_bt_tx_volume_from_nvs_once();
        s_bt_tx_volume = value;
        s_bt_tx_volume_known = true;
        audio_set_volume(BT_TX_PLAYER_FIXED_VOLUME);
        sync_ui_volume_from_route_state();

        // 立即下发改善交互；若模块随后输出 CLEAR OK，poll() 会再应用一次最新值。
        const bool queued = bt62sp_uart_debug_set_volume(value);
        LOGD("[音量] 蓝牙模块音量=%u 播放器固定=%u 命令排队=%d，退出蓝牙时写入 NVS",
             (unsigned)value,
             (unsigned)BT_TX_PLAYER_FIXED_VOLUME,
             queued ? 1 : 0);
        return queued;
    }

    s_saved_normal_volume = value;
    audio_set_volume(value);
    sync_ui_volume_from_route_state();
    LOGD("[音量] 播放器音量=%u", (unsigned)value);
    return true;
}

bool audio_output_route_step_user_volume_from_audio_task(int delta)
{
    if (delta < -100) delta = -100;
    if (delta > 100) delta = 100;
    if (delta == 0) return true;

    int value = static_cast<int>(audio_output_route_get_user_volume()) + delta;
    if (value < 0) value = 0;
    if (value > 100) value = 100;
    return audio_output_route_set_user_volume_from_audio_task(static_cast<uint8_t>(value));
}
