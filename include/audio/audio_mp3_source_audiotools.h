#pragma once

#include <stdint.h>

#include "audio/audio_mp3_source.h"

// HTTP 音源运行快照。WiFiClient 仅允许 AudioTask 所在线程访问；
// 其他任务只能读取这里发布的缓存状态，禁止直接调用 available()/connected()。
struct AudioMp3HttpSourceSnapshot {
  bool open = false;
  bool transport_connected = false;
  bool waiting_for_data = false;
  bool eof = false;
  uint32_t available_bytes = 0;
  uint32_t last_data_ms = 0;
  uint32_t last_update_ms = 0;
};

// 创建新的网络音频操作编号。新编号会使上一条正在连接或读取的网络流尽快退出。
uint32_t audio_mp3_audiotools_source_begin_operation();
void audio_mp3_audiotools_source_cancel_operation();
bool audio_mp3_audiotools_source_operation_is_current(uint32_t operation_id);

// 在 AudioTask 内快速读取 HTTP 响应体前 10 字节，计算 ID3v2 标签后的音频起始偏移。
// 探测失败时返回 false，调用方应回退普通 URL 起播。
bool audio_mp3_audiotools_source_probe_audio_start_offset(const char* url,
                                                           uint32_t operation_id,
                                                           uint32_t* out_offset);

bool audio_mp3_audiotools_source_open(const char* url, uint32_t operation_id, AudioMp3Source& out_source);
// HTTP MP3 文件从指定字节偏移起播。offset>0 时会发送 Range: bytes=offset-，
// 仅在服务器返回 206 Partial Content 时成功；失败时由上层回退普通 URL 起播。
bool audio_mp3_audiotools_source_open_from_offset(const char* url, uint32_t start_offset, uint32_t operation_id, AudioMp3Source& out_source);
void audio_mp3_audiotools_source_close();

// 只读取缓存快照，不接触 WiFiClient，可由 UI/状态任务安全调用。
bool audio_mp3_audiotools_source_get_snapshot(AudioMp3HttpSourceSnapshot* out_snapshot);