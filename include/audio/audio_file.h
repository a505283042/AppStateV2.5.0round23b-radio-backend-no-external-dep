#pragma once
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <SdFat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "storage/storage_io.h"

enum AudioFileDirCacheReason : uint8_t {
  AUDIO_DIRCACHE_UNKNOWN = 0,
  AUDIO_DIRCACHE_HIT_LAST = 1,
  AUDIO_DIRCACHE_HIT_MUSIC_ROOT = 2,
  AUDIO_DIRCACHE_MISS_FIRST = 3,
  AUDIO_DIRCACHE_MISS_CHANGED = 4,
  AUDIO_DIRCACHE_MISS_INVALID = 5,
  AUDIO_DIRCACHE_MISS_OPEN_FAILED = 6,
};

struct AudioFileOpenStats {
  uint32_t lock_wait_ms = 0;
  uint32_t dir_prepare_ms = 0;
  uint32_t file_open_ms = 0;
  uint32_t file_size_ms = 0;
  uint8_t used_dir_cache = 0;
  AudioFileDirCacheReason dir_cache_reason = AUDIO_DIRCACHE_UNKNOWN;
};

struct AudioFile {
  File32 f;
  uint32_t _cached_size;  // 缓存文件大小，避免频繁查询
  AudioFileOpenStats _last_open_stats;

  bool open(SdFat& sd, const char* path);
  void close();
  ssize_t read(void* dst, size_t bytes);   // 返回实际读到字节数（使用 ssize_t 避免溢出）
  bool seek(uint32_t pos);
  uint32_t tell();
  uint32_t size();

  const AudioFileOpenStats& last_open_stats() const { return _last_open_stats; }
};

void audio_file_invalidate_dir_cache();


const char* audio_file_dir_cache_reason_str(AudioFileDirCacheReason reason);
bool audio_file_prepare_music_root_cache();
