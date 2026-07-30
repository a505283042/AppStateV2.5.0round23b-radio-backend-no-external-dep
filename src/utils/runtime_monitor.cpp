#include "utils/runtime_monitor.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp32-hal-psram.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "audio/audio_service.h"
#include "player_assets.h"
#include "ui/ui.h"
#include "utils/log.h"

#ifndef RUNTIME_MONITOR_PERIOD_MS
#define RUNTIME_MONITOR_PERIOD_MS 15000 // 运行时监控周期：15 秒
#endif

#ifndef RUNTIME_MONITOR_FIRST_DELAY_MS
#define RUNTIME_MONITOR_FIRST_DELAY_MS 6000 // 首次监控延迟：避开开机初始化阶段的日志高峰
#endif

#ifndef RUNTIME_MONITOR_WARN_STACK_MARGIN_PCT
#define RUNTIME_MONITOR_WARN_STACK_MARGIN_PCT 25 // 栈余量低于 25% 才在 INFO 模式下告警
#endif

#ifndef RUNTIME_MONITOR_WARN_INTERNAL_FREE_BYTES
#define RUNTIME_MONITOR_WARN_INTERNAL_FREE_BYTES 81920 // 内部 RAM 低于 80KB 才告警
#endif

#ifndef RUNTIME_MONITOR_WARN_DMA_FREE_BYTES
#define RUNTIME_MONITOR_WARN_DMA_FREE_BYTES 65536 // DMA RAM 低于 64KB 才告警
#endif

// 以下数值必须与各任务创建处保持一致。运行时监控只读取任务栈水位，不持有任务句柄。
static constexpr uint32_t kAudioTaskStackBytes = 10240;
static constexpr uint32_t kUiTaskStackBytes = 4096;
static constexpr uint32_t kLoopTaskStackBytes = 8192; // 与 platformio.ini 的 ARDUINO_LOOP_STACK_SIZE 保持一致
static constexpr uint32_t kNetCoverTaskStackBytes = 12288;
static constexpr uint32_t kFlacNetTaskStackBytes = 8192;
static constexpr uint32_t kRescanTaskStackBytes = 8192;

static bool s_runtime_monitor_started = false;
static uint32_t s_runtime_monitor_next_ms = 0;

// 计算内存碎片百分比
static uint32_t calc_fragment_percent(uint32_t free_bytes, uint32_t largest_block)
{
  if (free_bytes == 0 || largest_block >= free_bytes) return 0;
  return 100u - (largest_block * 100u) / free_bytes;
}

static void log_task_stack_usage(const char* name, TaskHandle_t handle, uint32_t configured_stack_bytes)
{
  if (!handle || configured_stack_bytes == 0) return;

  const uint32_t min_free_bytes = (uint32_t)uxTaskGetStackHighWaterMark(handle);
  const uint32_t peak_used_bytes =
      (configured_stack_bytes > min_free_bytes) ? (configured_stack_bytes - min_free_bytes) : 0;
  const uint32_t margin_pct =
      (uint32_t)((100ULL * min_free_bytes) / configured_stack_bytes);

  LOGD("[监控][栈] %s stack=%uB min_可用=%uB peak_used=%uB 余量=%u%%",
       name,
       (unsigned)configured_stack_bytes,
       (unsigned)min_free_bytes,
       (unsigned)peak_used_bytes,
       (unsigned)margin_pct);

  if (margin_pct <= RUNTIME_MONITOR_WARN_STACK_MARGIN_PCT) {
    LOGW("[监控][栈] 偏低 余量: %s stack=%uB min_可用=%uB peak_used=%uB 余量=%u%%",
         name,
         (unsigned)configured_stack_bytes,
         (unsigned)min_free_bytes,
         (unsigned)peak_used_bytes,
         (unsigned)margin_pct);
  }
}

static void runtime_monitor_log_snapshot()
{
  const uint32_t free_heap = (uint32_t)ESP.getFreeHeap();
  const uint32_t min_free_heap = (uint32_t)ESP.getMinFreeHeap();
  const uint32_t largest_heap = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  const uint32_t heap_frag = calc_fragment_percent(free_heap, largest_heap);

  const uint32_t free_internal = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const uint32_t largest_internal = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const uint32_t internal_frag = calc_fragment_percent(free_internal, largest_internal);

  const uint32_t free_dma = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  const uint32_t largest_dma = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  const uint32_t dma_frag = calc_fragment_percent(free_dma, largest_dma);

  const bool has_psram = psramFound();
  const uint32_t free_psram = has_psram ? (uint32_t)ESP.getFreePsram() : 0u;
  const uint32_t largest_psram = has_psram
      ? (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
      : 0u;
  const uint32_t psram_frag = has_psram ? calc_fragment_percent(free_psram, largest_psram) : 0u;

  LOGD("[监控][内存] 堆 可用=%lu min=%lu 最大连续=%lu 碎片=%lu%%",
       (unsigned long)free_heap,
       (unsigned long)min_free_heap,
       (unsigned long)largest_heap,
       (unsigned long)heap_frag);

  LOGD("[监控][内存] 内部 RAM 可用=%lu 最大连续=%lu 碎片=%lu%% | DMA RAM 可用=%lu 最大连续=%lu 碎片=%lu%%",
       (unsigned long)free_internal,
       (unsigned long)largest_internal,
       (unsigned long)internal_frag,
       (unsigned long)free_dma,
       (unsigned long)largest_dma,
       (unsigned long)dma_frag);

  LOGD("[监控][内存] psram 可用=%lu 最大连续=%lu 碎片=%lu%%",
       (unsigned long)free_psram,
       (unsigned long)largest_psram,
       (unsigned long)psram_frag);

  if (free_internal < RUNTIME_MONITOR_WARN_INTERNAL_FREE_BYTES) {
    LOGW("[监控][内存] 内部 RAM 偏低: 可用=%lu 最大连续=%lu",
         (unsigned long)free_internal,
         (unsigned long)largest_internal);
  }
  if (free_dma < RUNTIME_MONITOR_WARN_DMA_FREE_BYTES) {
    LOGW("[监控][内存] 偏低 DMA RAM: 可用=%lu 最大连续=%lu",
         (unsigned long)free_dma,
         (unsigned long)largest_dma);
  }

  log_task_stack_usage("AudioTask", audio_service_get_task_handle(), kAudioTaskStackBytes);
  log_task_stack_usage("UiTask", ui_get_task_handle(), kUiTaskStackBytes);
  log_task_stack_usage("loopTask(含监控)", xTaskGetHandle("loopTask"), kLoopTaskStackBytes);
  log_task_stack_usage("PlayerAssetTask", xTaskGetHandle("PlayerAssetTask"), PLAYER_ASSET_TASK_STACK_BYTES);
  log_task_stack_usage("NetCoverTask", xTaskGetHandle("NetCoverTask"), kNetCoverTaskStackBytes);
  log_task_stack_usage("FlacNetTask", xTaskGetHandle("FlacNetTask"), kFlacNetTaskStackBytes);
  log_task_stack_usage("rescan_v3", xTaskGetHandle("rescan_v3"), kRescanTaskStackBytes);
}

bool runtime_monitor_start(void)
{
  if (s_runtime_monitor_started) return true;

  s_runtime_monitor_started = true;
  s_runtime_monitor_next_ms = millis() + RUNTIME_MONITOR_FIRST_DELAY_MS;
  LOGD("[监控] RuntimeMon 已并入 loopTask：首次延迟=%ums 周期=%ums",
       (unsigned)RUNTIME_MONITOR_FIRST_DELAY_MS,
       (unsigned)RUNTIME_MONITOR_PERIOD_MS);
  return true;
}

void runtime_monitor_update(void)
{
  if (!s_runtime_monitor_started) return;

  const uint32_t now = millis();
  if ((int32_t)(now - s_runtime_monitor_next_ms) < 0) return;

  // 先安排下一次时间，再输出日志；即使串口短暂阻塞，也不会在下一轮补跑多次。
  s_runtime_monitor_next_ms = now + RUNTIME_MONITOR_PERIOD_MS;
  runtime_monitor_log_snapshot();
}
