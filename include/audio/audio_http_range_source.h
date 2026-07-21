#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

// HTTP Range 可寻址音源运行快照。AudioTask 只调用公开读写接口，
// 模块内部的 FlacNetTask 独占 WiFiClient 并持续预取；UI、Web 和其它任务只能读取快照。
struct AudioHttpRangeSourceSnapshot {
  bool open = false;
  bool transport_connected = false;
  bool waiting_for_data = false;
  bool reconnecting = false;
  bool eof = false;
  bool prefetch_complete = false;  // 网络端已把剩余文件全部写入环形缓冲
  uint8_t retry_attempt = 0;
  uint32_t retry_delay_ms = 0;
  uint32_t available_bytes = 0;
  uint32_t cached_bytes = 0;
  uint32_t transport_available_bytes = 0;
  uint32_t cache_capacity_bytes = 0;
  uint32_t last_data_ms = 0;
  uint32_t current_offset = 0;
  uint32_t total_size = 0;
  uint32_t range_open_count = 0;
  uint32_t reconnect_attempt_count = 0;
  uint32_t reconnect_success_count = 0;
};

// 打开支持 HTTP Range 的文件。当前第一阶段只支持 http://。
bool audio_http_range_source_open(const char* url, uint32_t operation_id);
void audio_http_range_source_close();

// read 返回实际字节数；0 表示自然 EOF；-1 表示取消、网络或协议错误。
ssize_t audio_http_range_source_read(void* dst, size_t bytes);
bool audio_http_range_source_seek(uint32_t absolute_offset);
uint32_t audio_http_range_source_tell();
uint32_t audio_http_range_source_size();
bool audio_http_range_source_had_io_error();
bool audio_http_range_source_is_open();

// 最近一次稳定错误码，调用方不得释放。
const char* audio_http_range_source_get_last_error();

// 只读取缓存快照，不访问 WiFiClient。
bool audio_http_range_source_get_snapshot(AudioHttpRangeSourceSnapshot* out_snapshot);
