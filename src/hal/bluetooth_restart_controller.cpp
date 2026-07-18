#include "hal/bluetooth_restart_controller.h"

#include "audio/audio_output_route.h"
#include "hal/board_hw_control.h"
#include "utils/log.h"

#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <freertos/task.h>

namespace {

struct BluetoothRestartState {
    bool in_progress = false;
    bool cancel_requested = false;
    bool restore_mode = false;
    BluetoothRestartPolicy policy = BluetoothRestartPolicy::PreserveCurrentMode;
    BluetoothRestartResult last_result = BluetoothRestartResult::None;
    uint32_t revision = 0;
};

BluetoothRestartState s_state{};
portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;

void state_revision_advance_locked()
{
    ++s_state.revision;
    if (s_state.revision == 0) {
        ++s_state.revision;
    }
}

BluetoothRestartState state_get()
{
    portENTER_CRITICAL(&s_state_mux);
    const BluetoothRestartState snapshot = s_state;
    portEXIT_CRITICAL(&s_state_mux);
    return snapshot;
}

const char* policy_label(BluetoothRestartPolicy policy)
{
    return policy == BluetoothRestartPolicy::RequireBluetoothTxRoute
        ? "音频输出"
        : "蓝牙设置";
}

void finish_restart(BluetoothRestartResult result)
{
    portENTER_CRITICAL(&s_state_mux);
    s_state.in_progress = false;
    s_state.cancel_requested = false;
    s_state.last_result = result;
    state_revision_advance_locked();
    portEXIT_CRITICAL(&s_state_mux);
}

void bluetooth_restart_task(void*)
{
    const BluetoothRestartState request = state_get();
    const char* source = policy_label(request.policy);

    if (request.cancel_requested) {
        LOGW("[蓝牙重启] 已取消：任务启动前收到取消请求");
        finish_restart(BluetoothRestartResult::Cancelled);
        vTaskDelete(nullptr);
        return;
    }

    LOGI("[蓝牙重启] 开始：来源=%s 恢复模式=%s",
         source,
         request.restore_mode ? "发射" : "接收");

    if (!board_hw_set_bt_power(false)) {
        LOGW("[蓝牙重启] 失败：无法关闭蓝牙电源");
        finish_restart(BluetoothRestartResult::PowerOffFailed);
        vTaskDelete(nullptr);
        return;
    }

    // 保持足够的断电时间，让 BT62SP 完成真正复位。
    vTaskDelay(pdMS_TO_TICKS(300));

    if (state_get().cancel_requested) {
        LOGW("[蓝牙重启] 已取消：保持蓝牙断电，不再重新上电");
        finish_restart(BluetoothRestartResult::Cancelled);
        vTaskDelete(nullptr);
        return;
    }

    if (request.policy == BluetoothRestartPolicy::RequireBluetoothTxRoute &&
        !audio_output_route_is_bluetooth_tx()) {
        LOGW("[蓝牙重启] 已取消重新上电：输出路线已离开蓝牙发射");
        finish_restart(BluetoothRestartResult::RouteChanged);
        vTaskDelete(nullptr);
        return;
    }

    const bool mode_ok = board_hw_set_bt_mode(request.restore_mode);
    const bool power_on_ok = board_hw_set_bt_power(true);
    bool route_ok = true;

    if (power_on_ok &&
        request.policy == BluetoothRestartPolicy::RequireBluetoothTxRoute) {
        // 通过 AudioTask 重新施加蓝牙发射路线约束和音量策略。
        route_ok = audio_output_route_enforce();
    }

    BluetoothRestartResult result = BluetoothRestartResult::Success;
    if (!mode_ok) {
        result = BluetoothRestartResult::ModeRestoreFailed;
    } else if (!power_on_ok) {
        result = BluetoothRestartResult::PowerOnFailed;
    } else if (!route_ok) {
        result = BluetoothRestartResult::RouteEnforceFailed;
    }

    if (result == BluetoothRestartResult::Success) {
        LOGI("[蓝牙重启] 完成：来源=%s 电源开启=1 模式恢复=1 路线恢复=%d",
             source,
             route_ok ? 1 : 0);
    } else {
        LOGW("[蓝牙重启] 完成但存在失败：来源=%s 模式恢复=%d 电源开启=%d 路线恢复=%d 结果=%u",
             source,
             mode_ok ? 1 : 0,
             power_on_ok ? 1 : 0,
             route_ok ? 1 : 0,
             static_cast<unsigned>(result));
    }

    finish_restart(result);
    vTaskDelete(nullptr);
}

} // namespace

BluetoothRestartSnapshot bluetooth_restart_snapshot_get()
{
    const BluetoothRestartState state = state_get();

    BluetoothRestartSnapshot snapshot{};
    snapshot.in_progress = state.in_progress;
    snapshot.cancel_requested = state.cancel_requested;
    snapshot.policy = state.policy;
    snapshot.last_result = state.last_result;
    snapshot.revision = state.revision;
    return snapshot;
}

bool bluetooth_restart_is_in_progress()
{
    return bluetooth_restart_snapshot_get().in_progress;
}

bool bluetooth_restart_request(BluetoothRestartPolicy policy)
{
    if (!board_hw_get_bt_power()) {
        LOGW("[蓝牙重启] 请求已忽略：蓝牙电源关闭");
        return false;
    }

    if (policy == BluetoothRestartPolicy::RequireBluetoothTxRoute &&
        !audio_output_route_is_bluetooth_tx()) {
        LOGW("[蓝牙重启] 请求已忽略：当前不是蓝牙发射路线");
        return false;
    }

    const bool restore_mode = board_hw_get_bt_mode();

    portENTER_CRITICAL(&s_state_mux);
    if (s_state.in_progress) {
        portEXIT_CRITICAL(&s_state_mux);
        LOGW("[蓝牙重启] 请求已忽略：已有重启任务正在执行");
        return false;
    }

    s_state.in_progress = true;
    s_state.cancel_requested = false;
    s_state.restore_mode = restore_mode;
    s_state.policy = policy;
    s_state.last_result = BluetoothRestartResult::None;
    state_revision_advance_locked();
    portEXIT_CRITICAL(&s_state_mux);

    const BaseType_t created = xTaskCreate(
        bluetooth_restart_task,
        "bt_restart",
        2048,
        nullptr,
        1,
        nullptr);

    if (created != pdPASS) {
        portENTER_CRITICAL(&s_state_mux);
        s_state.in_progress = false;
        s_state.cancel_requested = false;
        s_state.last_result = BluetoothRestartResult::TaskCreateFailed;
        state_revision_advance_locked();
        portEXIT_CRITICAL(&s_state_mux);

        LOGW("[蓝牙重启] 创建重启任务失败");
        return false;
    }

    return true;
}

bool bluetooth_restart_cancel()
{
    portENTER_CRITICAL(&s_state_mux);
    if (!s_state.in_progress) {
        portEXIT_CRITICAL(&s_state_mux);
        return false;
    }

    if (!s_state.cancel_requested) {
        s_state.cancel_requested = true;
        state_revision_advance_locked();
    }
    portEXIT_CRITICAL(&s_state_mux);

    LOGW("[蓝牙重启] 已发送取消请求");
    return true;
}
