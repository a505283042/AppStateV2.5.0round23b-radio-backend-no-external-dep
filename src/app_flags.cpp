#include "app_flags.h"

#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

#include "utils/log.h"

namespace {

AppRescanState s_rescan_state{};
portMUX_TYPE s_rescan_mux = portMUX_INITIALIZER_UNLOCKED;

AppPlayModeSnapshot s_play_mode_state{};
portMUX_TYPE s_play_mode_mux = portMUX_INITIALIZER_UNLOCKED;

bool play_mode_is_valid(play_mode_t mode)
{
  const int raw = static_cast<int>(mode);
  return raw >= static_cast<int>(PLAY_MODE_ALL_SEQ) &&
         raw <= static_cast<int>(PLAY_MODE_ALBUM_RND);
}

const char* play_mode_reason_label(AppPlayModeChangeReason reason)
{
  switch (reason) {
    case AppPlayModeChangeReason::PlayerControl:   return "播放器控制";
    case AppPlayModeChangeReason::RemoteNormalize:return "网络音源归一";
    case AppPlayModeChangeReason::NfcBinding:      return "NFC绑定";
    case AppPlayModeChangeReason::SnapshotRestore: return "快照恢复";
    case AppPlayModeChangeReason::WebControl:      return "Web控制";
    case AppPlayModeChangeReason::Internal:
    default:                                       return "内部";
  }
}

} // namespace

AppRescanState app_rescan_state_get()
{
  portENTER_CRITICAL(&s_rescan_mux);
  const AppRescanState snapshot = s_rescan_state;
  portEXIT_CRITICAL(&s_rescan_mux);
  return snapshot;
}

bool app_rescan_begin()
{
  portENTER_CRITICAL(&s_rescan_mux);
  if (s_rescan_state.rescanning) {
    portEXIT_CRITICAL(&s_rescan_mux);
    return false;
  }

  s_rescan_state.rescanning = true;
  s_rescan_state.done = false;
  s_rescan_state.success = false;
  s_rescan_state.abort_requested = false;
  portEXIT_CRITICAL(&s_rescan_mux);
  return true;
}

void app_rescan_mark_finished(bool success)
{
  portENTER_CRITICAL(&s_rescan_mux);
  s_rescan_state.success = success;
  s_rescan_state.done = true;
  portEXIT_CRITICAL(&s_rescan_mux);
}

bool app_rescan_request_abort()
{
  portENTER_CRITICAL(&s_rescan_mux);
  if (!s_rescan_state.rescanning || s_rescan_state.done) {
    portEXIT_CRITICAL(&s_rescan_mux);
    return false;
  }

  const bool changed = !s_rescan_state.abort_requested;
  s_rescan_state.abort_requested = true;
  portEXIT_CRITICAL(&s_rescan_mux);
  return changed;
}

bool app_rescan_should_abort()
{
  portENTER_CRITICAL(&s_rescan_mux);
  const bool abort_requested = s_rescan_state.abort_requested;
  portEXIT_CRITICAL(&s_rescan_mux);
  return abort_requested;
}

bool app_rescan_consume_result(bool& success, bool& aborted)
{
  portENTER_CRITICAL(&s_rescan_mux);
  if (!s_rescan_state.done) {
    portEXIT_CRITICAL(&s_rescan_mux);
    return false;
  }

  success = s_rescan_state.success;
  aborted = s_rescan_state.abort_requested;
  s_rescan_state = AppRescanState{};
  portEXIT_CRITICAL(&s_rescan_mux);
  return true;
}

void app_rescan_reset()
{
  portENTER_CRITICAL(&s_rescan_mux);
  s_rescan_state = AppRescanState{};
  portEXIT_CRITICAL(&s_rescan_mux);
}

AppPlayModeSnapshot app_play_mode_snapshot_get()
{
  portENTER_CRITICAL(&s_play_mode_mux);
  const AppPlayModeSnapshot snapshot = s_play_mode_state;
  portEXIT_CRITICAL(&s_play_mode_mux);
  return snapshot;
}

play_mode_t app_play_mode_get()
{
  return app_play_mode_snapshot_get().mode;
}

bool app_play_mode_set(play_mode_t mode, AppPlayModeChangeReason reason)
{
  if (!play_mode_is_valid(mode)) {
    LOGW("[播放模式] 拒绝非法模式：%d 来源=%s",
         static_cast<int>(mode),
         play_mode_reason_label(reason));
    return false;
  }

  play_mode_t old_mode = PLAY_MODE_ALL_SEQ;
  bool changed = false;
  uint32_t revision = 0;

  portENTER_CRITICAL(&s_play_mode_mux);
  old_mode = s_play_mode_state.mode;
  changed = old_mode != mode;
  if (changed) {
    s_play_mode_state.mode = mode;
    ++s_play_mode_state.revision;
    if (s_play_mode_state.revision == 0) {
      ++s_play_mode_state.revision;
    }
  }
  revision = s_play_mode_state.revision;
  portEXIT_CRITICAL(&s_play_mode_mux);

  // 即使模式没有变化，也重新同步 UI，便于启动恢复和页面重建后校准显示。
  ui_set_play_mode(mode);

  if (changed) {
    LOGD("[播放模式] %d -> %d 来源=%s 版本=%lu",
         static_cast<int>(old_mode),
         static_cast<int>(mode),
         play_mode_reason_label(reason),
         static_cast<unsigned long>(revision));
  }
  return true;
}
