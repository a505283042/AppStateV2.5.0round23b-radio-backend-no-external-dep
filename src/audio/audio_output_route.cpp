#include "audio/audio_output_route.h"

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
// 该值只作为内部缓存初值；音量未知时 UI 显示“--”，绝不能把 50 当成真实调节基准。
static constexpr uint8_t BT_TX_VOLUME_CACHE_INITIAL = 50;
// BT62SP 上电后会先输出启动信息，等待更充分后再查询；整个过程仍为非阻塞状态机。
static constexpr uint32_t BT_TX_VOLUME_QUERY_SETTLE_MS = 2500;

volatile uint8_t s_bt_tx_volume = BT_TX_VOLUME_CACHE_INITIAL;
volatile uint8_t s_saved_normal_volume = 80;
volatile bool s_bt_tx_volume_known = false;
bool s_bt_tx_volume_policy_active = false;
uint32_t s_bt_tx_volume_query_id = 0;
int16_t s_bt_tx_pending_delta = 0;

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

    // 送入 BT62SP 模拟输入的播放器音量固定在安全值。
    // BT62SP 自身音量必须以上电查询结果为准，禁止在进入路线时写入默认值。
    audio_set_volume(BT_TX_PLAYER_FIXED_VOLUME);
    s_bt_tx_pending_delta = 0;
    sync_ui_volume_from_route_state();

    uint32_t request_id = 0;
    if (bt62sp_uart_debug_request_volume_query(BT_TX_VOLUME_QUERY_SETTLE_MS, &request_id)) {
        s_bt_tx_volume_query_id = request_id;
        LOGD("[音量] 蓝牙发射：播放器固定=%u，查询模块保存音量 请求=%lu 已知=%d 临时显示=%u",
             (unsigned)BT_TX_PLAYER_FIXED_VOLUME,
             (unsigned long)request_id,
             s_bt_tx_volume_known ? 1 : 0,
             (unsigned)s_bt_tx_volume);
    } else {
        s_bt_tx_volume_query_id = 0;
        LOGW("[音量] BT62SP 音量查询排队失败，保持模块当前音量，不写默认值");
    }
}

void restore_normal_volume_policy_if_needed()
{
    if (!s_bt_tx_volume_policy_active) {
        return;
    }

    bt62sp_uart_debug_cancel_volume_query();
    s_bt_tx_volume_query_id = 0;
    s_bt_tx_pending_delta = 0;

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
    Bt62spVolumeQueryEvent event{};
    if (!bt62sp_uart_debug_take_volume_query_event(&event)) {
        return;
    }

    // 只接收当前路线发起的查询；离开蓝牙路线或被用户调音取消后，迟到结果直接丢弃。
    if (!audio_output_route_is_bluetooth_tx() ||
        s_bt_tx_volume_query_id == 0 ||
        event.request_id != s_bt_tx_volume_query_id) {
        LOGD("[音量] 忽略过期 BT62SP 音量结果：事件请求=%lu 当前请求=%lu 路线=%s",
             (unsigned long)event.request_id,
             (unsigned long)s_bt_tx_volume_query_id,
             audio_output_route_label());
        return;
    }

    s_bt_tx_volume_query_id = 0;

    if (event.result == Bt62spVolumeQueryResult::Success) {
        const uint8_t queried = event.volume > 100 ? 100 : event.volume;
        s_bt_tx_volume = queried;
        s_bt_tx_volume_known = true;

        if (s_bt_tx_pending_delta != 0) {
            int adjusted = static_cast<int>(queried) + static_cast<int>(s_bt_tx_pending_delta);
            if (adjusted < 0) adjusted = 0;
            if (adjusted > 100) adjusted = 100;
            s_bt_tx_pending_delta = 0;
            s_bt_tx_volume = static_cast<uint8_t>(adjusted);
            const bool queued = bt62sp_uart_debug_set_volume(s_bt_tx_volume);
            LOGI("[音量] 查询到 BT62SP=%u，应用等待中的相对调节后=%u 命令排队=%d",
                 (unsigned)queried,
                 (unsigned)s_bt_tx_volume,
                 queued ? 1 : 0);
        } else {
            LOGI("[音量] 已同步 BT62SP 保存音量=%u，未写入默认值",
                 (unsigned)s_bt_tx_volume);
        }

        sync_ui_volume_from_route_state();
        return;
    }

    if (event.result == Bt62spVolumeQueryResult::Timeout) {
        const int pending_delta = s_bt_tx_pending_delta;
        s_bt_tx_pending_delta = 0;
        sync_ui_volume_from_route_state();
        LOGW("[音量] BT62SP 音量查询超时：保持模块原音量；音量仍显示未知，未应用等待增量=%d",
             pending_delta);
    }
}

bool audio_output_route_set_user_volume_from_audio_task(uint8_t value)
{
    if (value > 100) {
        value = 100;
    }

    if (audio_output_route_is_bluetooth_tx()) {
        // 绝对音量设置由用户明确指定，可以直接覆盖尚未完成的查询。
        s_bt_tx_volume_query_id = 0;
        s_bt_tx_pending_delta = 0;
        s_bt_tx_volume = value;
        s_bt_tx_volume_known = true;
        audio_set_volume(BT_TX_PLAYER_FIXED_VOLUME);
        sync_ui_volume_from_route_state();
        const bool queued = bt62sp_uart_debug_set_volume(value);
        LOGD("[音量] 蓝牙模块音量=%u 播放器固定=%u 命令排队=%d",
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

    if (audio_output_route_is_bluetooth_tx() && !s_bt_tx_volume_known) {
        int pending = static_cast<int>(s_bt_tx_pending_delta) + delta;
        if (pending < -100) pending = -100;
        if (pending > 100) pending = 100;
        s_bt_tx_pending_delta = static_cast<int16_t>(pending);
        sync_ui_volume_from_route_state();

        // 查询如果尚未排队或已经超时，旋钮操作会立即重新触发查询。
        if (s_bt_tx_volume_query_id == 0) {
            uint32_t request_id = 0;
            if (bt62sp_uart_debug_request_volume_query(0, &request_id)) {
                s_bt_tx_volume_query_id = request_id;
            }
        }

        LOGI("[音量] BT62SP 实际音量未知：暂存相对调节=%d，等待查询后应用",
             (int)s_bt_tx_pending_delta);
        return s_bt_tx_volume_query_id != 0;
    }

    int value = static_cast<int>(audio_output_route_get_user_volume()) + delta;
    if (value < 0) value = 0;
    if (value > 100) value = 100;
    return audio_output_route_set_user_volume_from_audio_task(static_cast<uint8_t>(value));
}
