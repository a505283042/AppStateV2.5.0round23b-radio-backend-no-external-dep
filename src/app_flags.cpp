#include "app_flags.h"

#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

namespace {

AppRescanState s_rescan_state{};
portMUX_TYPE s_rescan_mux = portMUX_INITIALIZER_UNLOCKED;

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
