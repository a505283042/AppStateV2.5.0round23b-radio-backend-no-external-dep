#include "storage/storage_io.h"
#include "utils/log.h"

SemaphoreHandle_t g_sd_mutex = nullptr;
static StaticSemaphore_t s_sd_mutex_storage{};

bool storage_sd_init_mutex(void)
{
  if (g_sd_mutex) return true;

  // 该函数在 boot_state 创建 AudioTask 前首次调用。使用静态互斥量，
  // 避免启动阶段因内部堆不足导致 SD 总线锁创建失败。
  g_sd_mutex =
      xSemaphoreCreateRecursiveMutexStatic(&s_sd_mutex_storage);
  if (!g_sd_mutex) {
    LOGE("[SD互斥锁] 创建递归 SD 互斥锁失败");
    return false;
  }
  LOGD("[SD互斥锁] 递归 SD 互斥锁已创建");
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
