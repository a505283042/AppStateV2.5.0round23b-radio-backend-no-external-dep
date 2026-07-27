#include "audio/audio_output_route.h"

#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

#include "audio/audio.h"
#include "audio/audio_service.h"
#include "hal/bt62sp_uart_debug.h"
#include "hal/board_hw_control.h"
#include "hal/i2c_bus_lock.h"
#include "ui/ui.h"
#include "utils/log.h"

namespace {

// 蓝牙发射模式下，BT62SP 的 L/R 是模拟输入。播放器音量过高会让模块输入级过载，
// 蓝牙音箱端听起来会发糊。因此进入蓝牙发射后固定播放器音量，只调 BT62SP AT+VOL。
static constexpr uint8_t BT_TX_PLAYER_FIXED_VOLUME = 20;
// UI、网页和旋钮始终使用 0..100 的逻辑音量。BT62SP 实测硬件音量 1..3 仍无声，
// 因此仅在 UART 边界把逻辑 1..100 映射到硬件 4..100；逻辑 0 仍发送硬件 0 静音。
static constexpr uint8_t BT_TX_VOLUME_DEFAULT = 50;
static constexpr uint8_t BT_TX_HARDWARE_MUTE_MAX = 3;
static constexpr uint8_t BT_TX_HARDWARE_MIN_AUDIBLE = 4;
static constexpr char BT_TX_PREFS_NAMESPACE[] = "audio_route";
// v1 保存的是直接发送给 BT62SP 的硬件值；v2 保存用户看到的逻辑音量。
static constexpr char BT_TX_PREFS_KEY_V1[] = "bt_vol";
static constexpr char BT_TX_PREFS_KEY_V2[] = "bt_vol_v2";

// 模块上电后可能先接收命令、稍后才完成音频运行态初始化。
// 先在固定等待后应用一次；收到 CLEAR OK 后再应用一次最新值。
static constexpr uint32_t BT_TX_VOLUME_APPLY_FALLBACK_MS = 2500;
static constexpr uint32_t BT_TX_VOLUME_APPLY_AFTER_READY_MS = 300;
static constexpr uint32_t BT_TX_VOLUME_APPLY_RETRY_MS = 100;

// 蓝牙发射射频与高瞬时负载会降低共享 I2C 的噪声裕量。
// 发射期间降到 50kHz，离开发射路线后恢复 100kHz。
static constexpr uint32_t I2C_CLOCK_NORMAL_HZ = 100000;
static constexpr uint32_t I2C_CLOCK_BT_TX_HZ = 50000;

struct AudioOutputRouteState {
    // 默认使用功放输出；3.5 耳机/Line out 始终常通，不受本状态控制。
    AudioOutputRoute route = AudioOutputRoute::Speaker;
    uint8_t bt_tx_volume = BT_TX_VOLUME_DEFAULT; // 用户逻辑音量，不是 AT+VOL 硬件值。
    uint8_t saved_normal_volume = 80;
    bool bt_tx_volume_known = false;
    bool bt_tx_volume_policy_active = false;
    bool bt_tx_nvs_loaded = false;
    bool bt_tx_nvs_valid = false;
    uint8_t bt_tx_persisted_volume = BT_TX_VOLUME_DEFAULT;
    bool bt_tx_apply_pending = false;
    uint32_t bt_tx_apply_due_ms = 0;
    uint32_t bt_tx_ready_generation_seen = 0;
    uint32_t revision = 1;
};

AudioOutputRouteState s_state{};
portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;

void state_revision_advance_locked()
{
    ++s_state.revision;
    if (s_state.revision == 0) {
        ++s_state.revision;
    }
}

AudioOutputRouteState state_snapshot_get()
{
    portENTER_CRITICAL(&s_state_mux);
    const AudioOutputRouteState snapshot = s_state;
    portEXIT_CRITICAL(&s_state_mux);
    return snapshot;
}

void publish_route(AudioOutputRoute route)
{
    portENTER_CRITICAL(&s_state_mux);
    if (s_state.route != route) {
        s_state.route = route;
        state_revision_advance_locked();
    }
    portEXIT_CRITICAL(&s_state_mux);
}

AudioOutputRoute current_route()
{
    return state_snapshot_get().route;
}

const char* route_label(AudioOutputRoute route)
{
    switch (route) {
        case AudioOutputRoute::HeadphoneOnly:
            return "仅耳机";
        case AudioOutputRoute::BluetoothTx:
            return "耳机+蓝牙";
        case AudioOutputRoute::Speaker:
        default:
            return "耳机+功放";
    }
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

bool verify_amp_control_state(bool expected_mute,
                              bool expected_shutdown,
                              const char* route_name)
{
    bool mute = false;
    bool shutdown = false;

    const bool mute_ok = board_hw_read_amp_mute(&mute);
    const bool shutdown_ok = board_hw_read_amp_shutdown(&shutdown);

    if (!mute_ok || !shutdown_ok) {
        LOGE("[音频输出] 功放控制脚读回失败：路线=%s MUTE读取=%d SHDN读取=%d",
             route_name ? route_name : "未知",
             mute_ok ? 1 : 0,
             shutdown_ok ? 1 : 0);
        return false;
    }

    if (mute != expected_mute || shutdown != expected_shutdown) {
        LOGE("[音频输出] 功放控制脚读回不匹配：路线=%s MUTE=%d/%d SHDN=%d/%d",
             route_name ? route_name : "未知",
             mute ? 1 : 0,
             expected_mute ? 1 : 0,
             shutdown ? 1 : 0,
             expected_shutdown ? 1 : 0);
        return false;
    }

    LOGI("[音频输出] 功放控制脚读回确认：路线=%s MUTE=%d SHDN=%d",
         route_name ? route_name : "未知",
         mute ? 1 : 0,
         shutdown ? 1 : 0);
    return true;
}

bool keep_safe_outputs_after_route_failure()
{
    // 路由切换失败时优先保证喇叭和蓝牙都不会意外出声。
    bool ok = true;
    ok = board_hw_set_amp_mute(true) && ok;
    ok = board_hw_set_amp_shutdown(true) && ok;
    ok = board_hw_set_bt_power(false) && ok;
    ok = board_hw_set_bt_mode(false) && ok;
    ok = i2c_bus_set_clock_hz(I2C_CLOCK_NORMAL_HZ) && ok;
    ok = verify_amp_control_state(true, true, "故障安全") && ok;
    return ok;
}

bool time_reached(uint32_t now, uint32_t target)
{
    return static_cast<int32_t>(now - target) >= 0;
}

// 保持 C++11 可用的单表达式 constexpr，方便编译期验证映射端点。
constexpr uint8_t bt_tx_logical_to_hardware(uint8_t logical_volume)
{
    return logical_volume == 0
        ? 0
        : (logical_volume >= 100
               ? 100
               : static_cast<uint8_t>(
                     BT_TX_HARDWARE_MIN_AUDIBLE +
                     ((static_cast<uint32_t>(logical_volume - 1) *
                       (100U - BT_TX_HARDWARE_MIN_AUDIBLE) + 49U) / 99U)));
}

constexpr uint8_t bt_tx_hardware_to_logical(uint8_t hardware_volume)
{
    return hardware_volume <= BT_TX_HARDWARE_MUTE_MAX
        ? 0
        : (hardware_volume >= 100
               ? 100
               : static_cast<uint8_t>(
                     1U +
                     ((static_cast<uint32_t>(hardware_volume - BT_TX_HARDWARE_MIN_AUDIBLE) *
                       99U + 48U) /
                      (100U - BT_TX_HARDWARE_MIN_AUDIBLE))));
}

static_assert(bt_tx_logical_to_hardware(0) == 0, "蓝牙静音映射错误");
static_assert(bt_tx_logical_to_hardware(1) == 4, "蓝牙最低可听音量映射错误");
static_assert(bt_tx_logical_to_hardware(100) == 100, "蓝牙最大音量映射错误");
static_assert(bt_tx_hardware_to_logical(3) == 0, "蓝牙硬件静音区迁移错误");
static_assert(bt_tx_hardware_to_logical(4) == 1, "蓝牙硬件最低可听档迁移错误");
static_assert(bt_tx_hardware_to_logical(100) == 100, "蓝牙硬件最大音量迁移错误");

void load_bt_tx_volume_from_nvs_once()
{
    bool should_load = false;

    portENTER_CRITICAL(&s_state_mux);
    if (!s_state.bt_tx_nvs_loaded) {
        s_state.bt_tx_nvs_loaded = true;
        s_state.bt_tx_nvs_valid = false;
        s_state.bt_tx_persisted_volume = BT_TX_VOLUME_DEFAULT;
        s_state.bt_tx_volume = BT_TX_VOLUME_DEFAULT;
        s_state.bt_tx_volume_known = false;
        state_revision_advance_locked();
        should_load = true;
    }
    portEXIT_CRITICAL(&s_state_mux);

    if (!should_load) {
        return;
    }

    uint8_t loaded_volume = BT_TX_VOLUME_DEFAULT;
    bool loaded_valid = false;

    Preferences pref;
    // 迁移旧 bt_vol 时需要写入 bt_vol_v2，因此这里以可写方式打开；正常启动不会重复写。
    if (!pref.begin(BT_TX_PREFS_NAMESPACE, false)) {
        LOGW("[音量] 打开蓝牙音量 NVS 失败，首次使用逻辑默认值=%u",
             (unsigned)BT_TX_VOLUME_DEFAULT);
    } else {
        if (pref.isKey(BT_TX_PREFS_KEY_V2)) {
            const uint8_t saved = pref.getUChar(BT_TX_PREFS_KEY_V2, BT_TX_VOLUME_DEFAULT);
            if (saved <= 100) {
                loaded_volume = saved;
                loaded_valid = true;
                LOGI("[音量] 已从 NVS 读取蓝牙逻辑音量=%u 硬件=%u",
                     (unsigned)loaded_volume,
                     (unsigned)bt_tx_logical_to_hardware(loaded_volume));
            } else {
                LOGW("[音量] NVS 蓝牙逻辑音量无效=%u，尝试迁移旧值",
                     (unsigned)saved);
            }
        }

        if (!loaded_valid && pref.isKey(BT_TX_PREFS_KEY_V1)) {
            const uint8_t old_hardware = pref.getUChar(BT_TX_PREFS_KEY_V1, BT_TX_VOLUME_DEFAULT);
            if (old_hardware <= 100) {
                loaded_volume = bt_tx_hardware_to_logical(old_hardware);
                loaded_valid = pref.putUChar(BT_TX_PREFS_KEY_V2, loaded_volume) > 0;
                LOGI("[音量] 已迁移蓝牙音量：旧硬件=%u -> 新逻辑=%u -> 硬件=%u 写入=%d",
                     (unsigned)old_hardware,
                     (unsigned)loaded_volume,
                     (unsigned)bt_tx_logical_to_hardware(loaded_volume),
                     loaded_valid ? 1 : 0);
            } else {
                LOGW("[音量] NVS 旧蓝牙硬件音量无效=%u，改用逻辑默认值=%u",
                     (unsigned)old_hardware,
                     (unsigned)BT_TX_VOLUME_DEFAULT);
            }
        }

        if (!pref.isKey(BT_TX_PREFS_KEY_V2) &&
            !pref.isKey(BT_TX_PREFS_KEY_V1)) {
            LOGI("[音量] NVS 尚无蓝牙音量，首次使用逻辑默认值=%u 硬件=%u",
                 (unsigned)BT_TX_VOLUME_DEFAULT,
                 (unsigned)bt_tx_logical_to_hardware(BT_TX_VOLUME_DEFAULT));
        }
        pref.end();
    }

    portENTER_CRITICAL(&s_state_mux);
    s_state.bt_tx_volume = loaded_volume;
    s_state.bt_tx_persisted_volume = loaded_volume;
    s_state.bt_tx_nvs_valid = loaded_valid;
    s_state.bt_tx_volume_known = true;
    state_revision_advance_locked();
    portEXIT_CRITICAL(&s_state_mux);
}

bool save_bt_tx_volume_to_nvs_if_needed()
{
    load_bt_tx_volume_from_nvs_once();

    const AudioOutputRouteState snapshot = state_snapshot_get();
    const uint8_t volume = snapshot.bt_tx_volume > 100 ? 100 : snapshot.bt_tx_volume;
    if (snapshot.bt_tx_nvs_valid && snapshot.bt_tx_persisted_volume == volume) {
        LOGD("[音量] 蓝牙发射音量未变化，跳过 NVS 写入=%u", (unsigned)volume);
        return true;
    }

    Preferences pref;
    if (!pref.begin(BT_TX_PREFS_NAMESPACE, false)) {
        LOGE("[音量] 保存蓝牙发射音量失败：打开 NVS namespace");
        return false;
    }

    const bool ok = pref.putUChar(BT_TX_PREFS_KEY_V2, volume) > 0;
    pref.end();

    if (!ok) {
        LOGE("[音量] 保存蓝牙发射音量失败：写入 NVS");
        return false;
    }

    portENTER_CRITICAL(&s_state_mux);
    s_state.bt_tx_persisted_volume = volume;
    s_state.bt_tx_nvs_valid = true;
    state_revision_advance_locked();
    portEXIT_CRITICAL(&s_state_mux);

    LOGI("[音量] 退出蓝牙发射，已保存逻辑音量到 NVS=%u 硬件=%u",
         (unsigned)volume,
         (unsigned)bt_tx_logical_to_hardware(volume));
    return true;
}

void schedule_bt_tx_volume_apply(uint32_t delay_ms)
{
    const uint32_t due_ms = millis() + delay_ms;

    portENTER_CRITICAL(&s_state_mux);
    s_state.bt_tx_apply_pending = true;
    s_state.bt_tx_apply_due_ms = due_ms;
    state_revision_advance_locked();
    portEXIT_CRITICAL(&s_state_mux);
}

void sync_ui_volume_from_route_state()
{
    const AudioOutputRouteState snapshot = state_snapshot_get();
    if (snapshot.route == AudioOutputRoute::BluetoothTx &&
        !snapshot.bt_tx_volume_known) {
        ui_set_volume_unknown();
        return;
    }

    ui_set_volume(snapshot.route == AudioOutputRoute::BluetoothTx
                      ? snapshot.bt_tx_volume
                      : audio_get_volume());
}

void apply_bluetooth_tx_volume_policy()
{
    bool policy_activated = false;
    uint8_t saved_normal_volume = audio_get_volume();
    if (saved_normal_volume > 100) {
        saved_normal_volume = 100;
    }

    portENTER_CRITICAL(&s_state_mux);
    if (!s_state.bt_tx_volume_policy_active) {
        s_state.saved_normal_volume = saved_normal_volume;
        s_state.bt_tx_volume_policy_active = true;
        state_revision_advance_locked();
        policy_activated = true;
    }
    portEXIT_CRITICAL(&s_state_mux);

    if (policy_activated) {
        LOGD("[音量] 进入蓝牙发射：保存播放器音量=%u",
             (unsigned)saved_normal_volume);
    }

    load_bt_tx_volume_from_nvs_once();

    // 送入 BT62SP 模拟输入的播放器音量固定在安全值。
    // BT62SP 不负责持久化音量；进入发射模式时由 ESP32 把 NVS 保存值主动下发。
    audio_set_volume(BT_TX_PLAYER_FIXED_VOLUME);
    bt62sp_uart_debug_cancel_volume_query();
    const uint32_t ready_generation = bt62sp_uart_debug_ready_generation();
    const uint32_t due_ms = millis() + BT_TX_VOLUME_APPLY_FALLBACK_MS;

    portENTER_CRITICAL(&s_state_mux);
    s_state.bt_tx_ready_generation_seen = ready_generation;
    s_state.bt_tx_apply_pending = true;
    s_state.bt_tx_apply_due_ms = due_ms;
    const uint8_t bt_volume = s_state.bt_tx_volume;
    state_revision_advance_locked();
    portEXIT_CRITICAL(&s_state_mux);

    LOGI("[音量] 蓝牙发射：播放器固定=%u，等待下发逻辑音量=%u 硬件=%u",
         (unsigned)BT_TX_PLAYER_FIXED_VOLUME,
         (unsigned)bt_volume,
         (unsigned)bt_tx_logical_to_hardware(bt_volume));
}

void restore_normal_volume_policy_if_needed()
{
    AudioOutputRouteState snapshot = state_snapshot_get();
    if (!snapshot.bt_tx_volume_policy_active) {
        return;
    }

    // 在关闭 BT62SP 电源前保存当前音量。只有数值变化时才写 NVS，避免无效擦写。
    (void)save_bt_tx_volume_to_nvs_if_needed();

    bt62sp_uart_debug_cancel_volume_query();
    const uint32_t ready_generation = bt62sp_uart_debug_ready_generation();
    const uint8_t restore = snapshot.saved_normal_volume > 100
        ? 100
        : snapshot.saved_normal_volume;

    portENTER_CRITICAL(&s_state_mux);
    s_state.bt_tx_apply_pending = false;
    s_state.bt_tx_apply_due_ms = 0;
    s_state.bt_tx_ready_generation_seen = ready_generation;
    s_state.bt_tx_volume_policy_active = false;
    state_revision_advance_locked();
    portEXIT_CRITICAL(&s_state_mux);

    audio_set_volume(restore);
    LOGD("[音量] 离开蓝牙发射：恢复播放器音量=%u", (unsigned)restore);
}

} // namespace

AudioOutputRouteSnapshot audio_output_route_snapshot_get()
{
    const AudioOutputRouteState state = state_snapshot_get();

    AudioOutputRouteSnapshot snapshot{};
    snapshot.route = state.route;
    snapshot.bluetooth_tx_volume = state.bt_tx_volume;
    snapshot.normal_volume = state.saved_normal_volume > 100
        ? 100
        : state.saved_normal_volume;
    snapshot.bluetooth_tx_volume_known = state.bt_tx_volume_known;
    snapshot.bluetooth_tx_policy_active = state.bt_tx_volume_policy_active;
    snapshot.bluetooth_tx_apply_pending = state.bt_tx_apply_pending;
    snapshot.revision = state.revision;
    return snapshot;
}

AudioOutputRoute audio_output_route_get()
{
    return audio_output_route_snapshot_get().route;
}

const char* audio_output_route_label()
{
    return route_label(audio_output_route_snapshot_get().route);
}

bool audio_output_route_is_headphone_only()
{
    return audio_output_route_snapshot_get().route == AudioOutputRoute::HeadphoneOnly;
}

bool audio_output_route_is_speaker()
{
    return audio_output_route_snapshot_get().route == AudioOutputRoute::Speaker;
}

bool audio_output_route_is_bluetooth_tx()
{
    return audio_output_route_snapshot_get().route == AudioOutputRoute::BluetoothTx;
}

uint8_t audio_output_route_bluetooth_tx_player_fixed_volume()
{
    return BT_TX_PLAYER_FIXED_VOLUME;
}

uint8_t audio_output_route_bluetooth_tx_volume()
{
    return audio_output_route_snapshot_get().bluetooth_tx_volume;
}

bool audio_output_route_bluetooth_tx_volume_known()
{
    return audio_output_route_snapshot_get().bluetooth_tx_volume_known;
}

uint8_t audio_output_route_get_user_volume()
{
    const AudioOutputRouteSnapshot snapshot = audio_output_route_snapshot_get();
    if (snapshot.route == AudioOutputRoute::BluetoothTx) {
        // 查询成功前没有可信的 BT62SP 绝对音量；返回正常路线保存值只用于快照/兼容读取，
        // UI 会单独显示“--”，相对调节也不会使用这个回退值。
        return snapshot.bluetooth_tx_volume_known
            ? snapshot.bluetooth_tx_volume
            : snapshot.normal_volume;
    }
    return audio_get_volume();
}

uint8_t audio_output_route_get_normal_volume()
{
    const AudioOutputRouteSnapshot snapshot = audio_output_route_snapshot_get();
    if (snapshot.route == AudioOutputRoute::BluetoothTx) {
        return snapshot.normal_volume;
    }
    const uint8_t volume = audio_get_volume();
    return volume > 100 ? 100 : volume;
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
    // 只有 AudioTask 可以直接操作输出硬件。必须在全部硬件操作和功放控制脚读回成功后，
    // 才发布新的对外路线；否则菜单不能显示“已切换”而实际功放仍处于未知状态。
    bool ok = true;

    if (route == AudioOutputRoute::BluetoothTx) {
        // 先关功放并读回确认，再打开蓝牙发射，避免功放与蓝牙同时耗电/出声。
        ok = board_hw_set_amp_mute(true) && ok;
        ok = board_hw_set_amp_shutdown(true) && ok;
        ok = verify_amp_control_state(true, true, "耳机+蓝牙") && ok;

        if (ok) {
            ok = i2c_bus_set_clock_hz(I2C_CLOCK_BT_TX_HZ) && ok;
            ok = board_hw_set_bt_mode(true) && ok;
            ok = board_hw_set_bt_power(true) && ok;
        }

        if (!ok) {
            if (keep_safe_outputs_after_route_failure()) {
                restore_normal_volume_policy_if_needed();
                publish_route(AudioOutputRoute::HeadphoneOnly);
                sync_ui_volume_from_route_state();
            }
            LOGE("[音频输出] 切到蓝牙失败：未发布蓝牙路线，已尝试回到仅耳机安全状态");
            return false;
        }

        apply_bluetooth_tx_volume_policy();
        publish_route(route);
        sync_ui_volume_from_route_state();
        LOGD("[音频输出] 路线=耳机+蓝牙，功放静音并关断，蓝牙切到发射模式并上电");
        return true;
    }

    // 离开蓝牙路线时先保存模块音量，但在硬件切换成功前不清除音量策略，
    // 避免失败后出现“仍显示蓝牙路线但播放器已经恢复普通音量”的中间状态。
    if (state_snapshot_get().bt_tx_volume_policy_active) {
        (void)save_bt_tx_volume_to_nvs_if_needed();
    }

    if (route == AudioOutputRoute::HeadphoneOnly) {
        // 仅耳机：关闭蓝牙，同时保持功放静音和关断。
        ok = board_hw_set_bt_power(false) && ok;
        ok = board_hw_set_bt_mode(false) && ok;
        ok = board_hw_set_amp_mute(true) && ok;
        ok = board_hw_set_amp_shutdown(true) && ok;
        ok = i2c_bus_set_clock_hz(I2C_CLOCK_NORMAL_HZ) && ok;
        ok = verify_amp_control_state(true, true, "仅耳机") && ok;

        if (!ok) {
            if (keep_safe_outputs_after_route_failure()) {
                restore_normal_volume_policy_if_needed();
                publish_route(AudioOutputRoute::HeadphoneOnly);
                sync_ui_volume_from_route_state();
            }
            LOGE("[音频输出] 切到仅耳机失败：已尝试保持仅耳机安全状态");
            return false;
        }

        restore_normal_volume_policy_if_needed();
        publish_route(route);
        sync_ui_volume_from_route_state();
        LOGD("[音频输出] 路线=仅耳机，功放静音并关断，蓝牙关闭并回到接收模式");
        return true;
    }

    // 切到功放时先解除关断但继续静音。是否取消静音由 AudioTask 根据播放/暂停状态决定。
    ok = board_hw_set_bt_power(false) && ok;
    ok = board_hw_set_bt_mode(false) && ok;
    ok = board_hw_set_amp_mute(true) && ok;
    ok = board_hw_set_amp_shutdown(false) && ok;
    ok = i2c_bus_set_clock_hz(I2C_CLOCK_NORMAL_HZ) && ok;
    ok = verify_amp_control_state(true, false, "耳机+功放") && ok;

    if (!ok) {
        if (keep_safe_outputs_after_route_failure()) {
            restore_normal_volume_policy_if_needed();
            publish_route(AudioOutputRoute::HeadphoneOnly);
            sync_ui_volume_from_route_state();
        }
        LOGE("[音频输出] 切到功放失败：未发布功放路线，已尝试回到仅耳机安全状态");
        return false;
    }

    restore_normal_volume_policy_if_needed();
    publish_route(AudioOutputRoute::Speaker);
    sync_ui_volume_from_route_state();
    LOGD("[音频输出] 路线=耳机+功放，功放已解除关断并保持静音");
    return true;
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
    AudioOutputRouteState snapshot = state_snapshot_get();
    if (snapshot.route != AudioOutputRoute::BluetoothTx) {
        return;
    }

    const uint32_t now = millis();
    const uint32_t ready_generation = bt62sp_uart_debug_ready_generation();

    if (ready_generation != 0 &&
        ready_generation != snapshot.bt_tx_ready_generation_seen) {
        bool scheduled = false;
        uint8_t volume = snapshot.bt_tx_volume;

        portENTER_CRITICAL(&s_state_mux);
        if (s_state.route == AudioOutputRoute::BluetoothTx &&
            ready_generation != s_state.bt_tx_ready_generation_seen) {
            s_state.bt_tx_ready_generation_seen = ready_generation;
            s_state.bt_tx_apply_pending = true;
            s_state.bt_tx_apply_due_ms = now + BT_TX_VOLUME_APPLY_AFTER_READY_MS;
            volume = s_state.bt_tx_volume;
            state_revision_advance_locked();
            scheduled = true;
        }
        portEXIT_CRITICAL(&s_state_mux);

        if (scheduled) {
            // CLEAR OK 表示模块音频运行态已经重新初始化。
            // 再次下发当前 NVS 音量，避免上电早期命令被模块忽略。
            LOGI("[音量] BT62SP 启动完成，准备重新应用 NVS 音量：代次=%lu 逻辑=%u 硬件=%u",
                 (unsigned long)ready_generation,
                 (unsigned)volume,
                 (unsigned)bt_tx_logical_to_hardware(volume));
        }
    }

    snapshot = state_snapshot_get();
    if (snapshot.route != AudioOutputRoute::BluetoothTx ||
        !snapshot.bt_tx_apply_pending ||
        !time_reached(now, snapshot.bt_tx_apply_due_ms)) {
        return;
    }

    const uint8_t logical_volume = snapshot.bt_tx_volume;
    const uint8_t hardware_volume = bt_tx_logical_to_hardware(logical_volume);
    if (bt62sp_uart_debug_set_volume(hardware_volume)) {
        portENTER_CRITICAL(&s_state_mux);
        if (s_state.route == AudioOutputRoute::BluetoothTx &&
            s_state.bt_tx_apply_pending &&
            s_state.bt_tx_volume == logical_volume) {
            s_state.bt_tx_apply_pending = false;
            s_state.bt_tx_apply_due_ms = 0;
            state_revision_advance_locked();
        }
        portEXIT_CRITICAL(&s_state_mux);

        sync_ui_volume_from_route_state();
        LOGI("[音量] 已向 BT62SP 应用 NVS 音量：逻辑=%u 硬件=%u",
             (unsigned)logical_volume,
             (unsigned)hardware_volume);
    } else {
        // UART 尚未就绪时短暂重试，保持非阻塞。
        portENTER_CRITICAL(&s_state_mux);
        if (s_state.route == AudioOutputRoute::BluetoothTx &&
            s_state.bt_tx_apply_pending) {
            s_state.bt_tx_apply_due_ms = now + BT_TX_VOLUME_APPLY_RETRY_MS;
            state_revision_advance_locked();
        }
        portEXIT_CRITICAL(&s_state_mux);
    }
}

bool audio_output_route_set_user_volume_from_audio_task(uint8_t value)
{
    if (value > 100) {
        value = 100;
    }

    const AudioOutputRoute route = current_route();
    if (route == AudioOutputRoute::BluetoothTx) {
        load_bt_tx_volume_from_nvs_once();

        portENTER_CRITICAL(&s_state_mux);
        s_state.bt_tx_volume = value;
        s_state.bt_tx_volume_known = true;
        state_revision_advance_locked();
        portEXIT_CRITICAL(&s_state_mux);

        audio_set_volume(BT_TX_PLAYER_FIXED_VOLUME);
        sync_ui_volume_from_route_state();

        // 立即下发改善交互；逻辑 1 已越过硬件 1..3 的无声区。
        // 若模块随后输出 CLEAR OK，poll() 会再应用一次最新逻辑值。
        const uint8_t hardware_volume = bt_tx_logical_to_hardware(value);
        const bool queued = bt62sp_uart_debug_set_volume(hardware_volume);
        if (!queued) {
            // UART 暂时不可用时保留最新值，由 AudioTask 的非阻塞轮询继续重试。
            schedule_bt_tx_volume_apply(BT_TX_VOLUME_APPLY_RETRY_MS);
        }
        LOGD("[音量] 蓝牙逻辑音量=%u 硬件=%u 播放器固定=%u 命令排队=%d，退出蓝牙时写入 NVS",
             (unsigned)value,
             (unsigned)hardware_volume,
             (unsigned)BT_TX_PLAYER_FIXED_VOLUME,
             queued ? 1 : 0);
        return queued;
    }

    portENTER_CRITICAL(&s_state_mux);
    if (s_state.saved_normal_volume != value) {
        s_state.saved_normal_volume = value;
        state_revision_advance_locked();
    }
    portEXIT_CRITICAL(&s_state_mux);

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
