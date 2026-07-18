#pragma once
#include <stdbool.h>
#include "ui/ui.h"

// 曲库重扫状态会被主循环、扫描任务、按键和 Web 任务共同访问。
// 使用值快照和封装接口，避免直接依赖 volatile 造成跨核心数据竞争。
struct AppRescanState {
  bool rescanning = false;
  bool done = false;
  bool success = false;
  bool abort_requested = false;
};

// 获取一致的重扫状态快照。
AppRescanState app_rescan_state_get();

// 开始一次新的重扫会话。已有重扫时返回 false。
bool app_rescan_begin();

// 扫描任务结束时发布结果；rescanning 会保持到主循环消费结果为止。
void app_rescan_mark_finished(bool success);

// 请求取消当前重扫。没有重扫或任务已经结束时返回 false。
bool app_rescan_request_abort();

// 扫描任务查询是否应该尽快退出。
bool app_rescan_should_abort();

// 原子消费一次完成事件，并清理本轮重扫状态。
bool app_rescan_consume_result(bool& success, bool& aborted);

// 创建扫描任务失败时回滚刚建立的重扫会话。
void app_rescan_reset();

// 播放模式由菜单、Web、NFC、快照恢复和播放器状态机共同访问，
// 统一通过快照接口读写，避免各模块直接修改全局变量后遗漏 UI 同步。
enum class AppPlayModeChangeReason : uint8_t {
  Internal = 0,
  PlayerControl,
  RemoteNormalize,
  NfcBinding,
  SnapshotRestore,
  WebControl,
};

struct AppPlayModeSnapshot {
  play_mode_t mode = PLAY_MODE_ALL_SEQ;
  uint32_t revision = 0;
};

// 获取播放模式和版本号的一致快照。
AppPlayModeSnapshot app_play_mode_snapshot_get();

// 获取当前播放模式。
play_mode_t app_play_mode_get();

// 设置播放模式并同步 UI。非法枚举返回 false；合法值即使与当前相同也会校准 UI。
bool app_play_mode_set(play_mode_t mode,
                       AppPlayModeChangeReason reason = AppPlayModeChangeReason::Internal);
