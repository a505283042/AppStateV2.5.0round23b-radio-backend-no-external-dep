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

    LOGI("[MON][STACK] %s stack=%uB min_free=%uB peak_used=%uB margin=%u%%", 
         name, 
         (unsigned)configured_stack_bytes, 
         (unsigned)min_free_bytes, 
         (unsigned)peak_used_bytes, 
         (unsigned)margin_pct);
}

// 运行时监控任务入口
static void runtime_monitor_task_entry(void*) 
{
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

    LOGI("[MON][MEM] heap free=%lu min=%lu largest=%lu frag=%lu%%",
         (unsigned long)free_heap,
         (unsigned long)min_free_heap,
         (unsigned long)largest_heap,
         (unsigned long)heap_frag);

    LOGI("[MON][MEM] internal free=%lu largest=%lu frag=%lu%% | dma free=%lu largest=%lu frag=%lu%%",
         (unsigned long)free_internal,
         (unsigned long)largest_internal,
         (unsigned long)internal_frag,
         (unsigned long)free_dma,
         (unsigned long)largest_dma,
         (unsigned long)dma_frag);

    LOGI("[MON][MEM] psram free=%lu largest=%lu frag=%lu%%",
         (unsigned long)free_psram,
         (unsigned long)largest_psram,
         (unsigned long)psram_frag);

    log_task_stack_usage("AudioTask", audio_service_get_task_handle(), 10240);
    log_task_stack_usage("UiTask", ui_get_task_handle(), 4096);
    log_task_stack_usage("loopTask", xTaskGetHandle("loopTask"), kLoopTaskStackBytes);
    log_task_stack_usage("RuntimeMon", s_runtime_monitor_task, kRuntimeMonStackBytes);
    log_task_stack_usage("PlayerAssetTask", xTaskGetHandle("PlayerAssetTask"), 5120);

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
