#pragma once

#include <stdint.h>

// 蓝牙模块重启策略。两个菜单页面共用同一个控制器，避免重复创建重启任务。
enum class BluetoothRestartPolicy : uint8_t {
    PreserveCurrentMode = 0,
    RequireBluetoothTxRoute = 1,
};

enum class BluetoothRestartResult : uint8_t {
    None = 0,
    Success,
    Cancelled,
    PowerOffFailed,
    RouteChanged,
    ModeRestoreFailed,
    PowerOnFailed,
    RouteEnforceFailed,
    TaskCreateFailed,
};

struct BluetoothRestartSnapshot {
    bool in_progress = false;
    bool cancel_requested = false;
    BluetoothRestartPolicy policy = BluetoothRestartPolicy::PreserveCurrentMode;
    BluetoothRestartResult last_result = BluetoothRestartResult::None;
    uint32_t revision = 0;
};

// 获取一致的重启状态快照。
BluetoothRestartSnapshot bluetooth_restart_snapshot_get();

// 是否已有蓝牙重启任务正在执行。
bool bluetooth_restart_is_in_progress();

// 请求重启蓝牙模块。模块必须已经上电；同一时间只允许一个重启任务。
bool bluetooth_restart_request(BluetoothRestartPolicy policy);

// 请求取消正在进行的重启。任务会保持蓝牙断电，不再重新上电。
bool bluetooth_restart_cancel();
