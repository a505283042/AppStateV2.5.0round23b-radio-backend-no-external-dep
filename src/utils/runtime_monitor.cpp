#include "utils/runtime_monitor.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp32-hal-psram.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "audio/audio_service.h"
#include "ui/ui.h"
#include "utils/log.h"

#ifndef RUNTIME_MONITOR_PERIOD_MS
#define RUNTIME_MONITOR_PERIOD_MS 15000 // 运行时监控任务周期：15 秒
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

#ifndef PLAYER_ASSET_TASK_STACK_BYTES
#define PLAYER_ASSET_TASK_STACK_BYTES 6144 // 需要与 player_assets.cpp 里的任务栈保持一致
#endif

static TaskHandle_t s_runtime_monitor_task = nullptr; // 运行时监控任务句柄

static constexpr uint32_t kRuntimeMonStackBytes = 3072; // 运行时监控任务栈大小
static constexpr uint32_t kLoopTaskStackBytes = 5120; // loopTask 任务栈大小

// 计算内存碎片百分比
static uint32_t calc_fragment_percent(uint32_t free_bytes, uint32_t largest_block) 
{
  if (free_bytes == 0 || largest_block >= free_bytes) return 0;
  return 100u - (largest_block * 100u) / free_bytes;
}

static void log_task_stack_usage(const char* name, TaskHandle_t handle, uint32_t configured_stack_bytes) {
    if (!handle || configured_stack_bytes == 0) return;

    const uint32_t min_free_bytes = (uint32_t)uxTaskGetStackHighWaterMark(handle);
    const uint32_t peak_used_bytes = 
        (configured_stack_bytes > min_free_bytes) ? (configured_stack_bytes - min_free_bytes) : 0;
    const uint32_t margin_pct = 
        (configured_stack_bytes > 0) ? (uint32_t)((100ULL * min_free_bytes) / configured_stack_bytes) : 0;

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

// 运行时监控任务入口
static void runtime_monitor_task_entry(void*) 
{
#if RUNTIME_MONITOR_FIRST_DELAY_MS > 0
  // 开机阶段 SD / UI / NFC / 曲库都在初始化，监控日志立即输出会把启动日志冲散。
  // 延迟第一次输出，只影响日志时间，不影响播放器功能。
  vTaskDelay(pdMS_TO_TICKS(RUNTIME_MONITOR_FIRST_DELAY_MS));
#endif

  for (;;) {
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

    log_task_stack_usage("AudioTask", audio_service_get_task_handle(), 10240);
    log_task_stack_usage("UiTask", ui_get_task_handle(), 4096);
    log_task_stack_usage("loopTask", xTaskGetHandle("loopTask"), kLoopTaskStackBytes);
    log_task_stack_usage("RuntimeMon", s_runtime_monitor_task, kRuntimeMonStackBytes);
    log_task_stack_usage("PlayerAssetTask", xTaskGetHandle("PlayerAssetTask"), PLAYER_ASSET_TASK_STACK_BYTES);

    TaskHandle_t rescan = xTaskGetHandle("rescan_v3");
    if (rescan) log_task_stack_usage("rescan_v3", rescan, 4096);

    vTaskDelay(pdMS_TO_TICKS(RUNTIME_MONITOR_PERIOD_MS));
  }
}

// 启动运行时监控任务
void runtime_monitor_start(void) 
{
  if (s_runtime_monitor_task) return;

  xTaskCreatePinnedToCore(runtime_monitor_task_entry,
                          "RuntimeMon",
                          kRuntimeMonStackBytes,
                          nullptr,
                          1,
                          &s_runtime_monitor_task,
                          1);
}
