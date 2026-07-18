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

extern volatile play_mode_t g_play_mode;  // 播放模式
