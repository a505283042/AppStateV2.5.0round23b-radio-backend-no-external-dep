#include "storage/storage_io.h"
#include "utils/log.h"

SemaphoreHandle_t g_sd_mutex = nullptr;

bool storage_sd_init_mutex(void)
{
  if (g_sd_mutex) return true;

  g_sd_mutex = xSemaphoreCreateRecursiveMutex();
  if (!g_sd_mutex) {
    LOGE("[SDIO] create recursive mutex failed");
    return false;
  }
  LOGI("[SDIO] recursive SD mutex created");
  return true;
}

bool storage_sd_lock(uint32_t timeout_ms)
{
  if (!storage_sd_init_mutex()) {
    return false;
  }

  TickType_t ticks = (timeout_ms == portMAX_DELAY)
      ? portMAX_DELAY
      : pdMS_TO_TICKS(timeout_ms);
  return xSemaphoreTakeRecursive(g_sd_mutex, ticks) == pdTRUE;
}

void storage_sd_unlock(void)
{
  if (!g_sd_mutex) return;
  xSemaphoreGiveRecursive(g_sd_mutex);
}
