#include "audio/audio_file.h"
#include "utils/log.h"
#include "storage/storage_io.h"

#include <string.h>

extern SdFat sd;

namespace {
constexpr size_t AUDIO_PATH_BUF = 300;
static File32 s_cached_dir;
static char s_cached_dir_path[AUDIO_PATH_BUF] = {0};
static bool s_cached_dir_valid = false;

static File32 s_music_root_dir;
static bool s_music_root_valid = false;

static bool is_music_root_path(const char* dir_path) {
  return dir_path && strcmp(dir_path, "/Music") == 0;
}

static void close_cached_dir_locked() {
  if (s_cached_dir) {
    s_cached_dir.close();
  }
  s_cached_dir_path[0] = '\0';
  s_cached_dir_valid = false;
}

static void close_music_root_locked() {
  if (s_music_root_dir) {
    s_music_root_dir.close();
  }
  s_music_root_valid = false;
}

static bool split_parent_dir(const char* path, char* out_dir, size_t out_dir_cap, const char** out_name) {
  if (!path || !out_dir || out_dir_cap == 0 || !out_name) return false;

  const char* slash = strrchr(path, '/');
  if (!slash) {
    out_dir[0] = '/';
    out_dir[1] = '\0';
    *out_name = path;
    return true;
  }

  *out_name = slash + 1;
  if (**out_name == '\0') return false;

  size_t dir_len = (size_t)(slash - path);
  if (dir_len == 0) {
    if (out_dir_cap < 2) return false;
    out_dir[0] = '/';
    out_dir[1] = '\0';
    return true;
  }

  if (dir_len + 1 > out_dir_cap) return false;
  memcpy(out_dir, path, dir_len);
  out_dir[dir_len] = '\0';
  return true;
}

static bool open_music_root_locked(SdFat& sd_ref) {
  if (s_music_root_valid && s_music_root_dir) return true;
  close_music_root_locked();
  if (!s_music_root_dir.open(&sd_ref, "/Music", O_RDONLY)) {
    return false;
  }
  s_music_root_valid = true;
  return true;
}

static bool ensure_cached_dir_locked(SdFat& sd_ref, const char* dir_path, AudioFileOpenStats& stats) {
  const uint32_t t0 = millis();

  if (is_music_root_path(dir_path) && open_music_root_locked(sd_ref)) {
    stats.used_dir_cache = 1;
    stats.dir_cache_reason = AUDIO_DIRCACHE_HIT_MUSIC_ROOT;
    stats.dir_prepare_ms = millis() - t0;
    return true;
  }

  if (s_cached_dir_valid && strcmp(s_cached_dir_path, dir_path) == 0 && s_cached_dir) {
    stats.used_dir_cache = 1;
    stats.dir_cache_reason = AUDIO_DIRCACHE_HIT_LAST;
    stats.dir_prepare_ms = millis() - t0;
    return true;
  }

  const bool had_prev = s_cached_dir_valid;
  const bool had_invalid = (s_cached_dir_valid && !s_cached_dir);
  close_cached_dir_locked();
  if (!s_cached_dir.open(&sd_ref, dir_path, O_RDONLY)) {
    stats.dir_cache_reason = AUDIO_DIRCACHE_MISS_OPEN_FAILED;
    stats.dir_prepare_ms = millis() - t0;
    return false;
  }

  strncpy(s_cached_dir_path, dir_path, sizeof(s_cached_dir_path) - 1);
  s_cached_dir_path[sizeof(s_cached_dir_path) - 1] = '\0';
  s_cached_dir_valid = true;
  stats.used_dir_cache = 0;
  if (had_invalid) {
    stats.dir_cache_reason = AUDIO_DIRCACHE_MISS_INVALID;
  } else if (had_prev) {
    stats.dir_cache_reason = AUDIO_DIRCACHE_MISS_CHANGED;
  } else {
    stats.dir_cache_reason = AUDIO_DIRCACHE_MISS_FIRST;
  }
  stats.dir_prepare_ms = millis() - t0;
  return true;
}
}

const char* audio_file_dir_cache_reason_str(AudioFileDirCacheReason reason) {
  switch (reason) {
    case AUDIO_DIRCACHE_HIT_LAST: return "hit_last";
    case AUDIO_DIRCACHE_HIT_MUSIC_ROOT: return "hit_music_root";
    case AUDIO_DIRCACHE_MISS_FIRST: return "miss_first";
    case AUDIO_DIRCACHE_MISS_CHANGED: return "miss_changed";
    case AUDIO_DIRCACHE_MISS_INVALID: return "miss_invalid";
    case AUDIO_DIRCACHE_MISS_OPEN_FAILED: return "miss_open_failed";
    default: return "unknown";
  }
}

bool audio_file_prepare_music_root_cache() {
  StorageSdLockGuard sd_lock(500);
  if (!sd_lock) {
    LOGW("[AudioFile] prepare music root cache lock timeout");
    return false;
  }
  const bool ok = open_music_root_locked(sd);
  LOGI("[AudioFile] prepare music root cache ok=%d", ok ? 1 : 0);
  return ok;
}

void audio_file_invalidate_dir_cache() {
  StorageSdLockGuard sd_lock(500);
  if (!sd_lock) {
    LOGW("[AudioFile] invalidate dir cache lock timeout");
    return;
  }
  close_cached_dir_locked();
  close_music_root_locked();
}

bool AudioFile::open(SdFat& sd_ref, const char* path) {
  _last_open_stats = {};
  const uint32_t t_lock_begin = millis();
  StorageSdLockGuard sd_lock(500);
  const uint32_t t_after_lock = millis();
  _last_open_stats.lock_wait_ms = t_after_lock - t_lock_begin;
  if (!sd_lock) {
    LOGE("[AudioFile] open lock timeout");
    return false;
  }

  if (f) {
    f.close();
  }

  char dir_path[AUDIO_PATH_BUF];
  const char* file_name = nullptr;
  if (!split_parent_dir(path, dir_path, sizeof(dir_path), &file_name)) {
    LOGE("[AudioFile] split path failed: %s", path ? path : "(null)");
    return false;
  }

  if (!ensure_cached_dir_locked(sd_ref, dir_path, _last_open_stats)) {
    LOGE("[AudioFile] open dir failed: %s", dir_path);
    return false;
  }

  File32* parent = nullptr;
  if (is_music_root_path(dir_path) && s_music_root_valid && s_music_root_dir) {
    parent = &s_music_root_dir;
  } else {
    parent = &s_cached_dir;
  }

  const uint32_t t_before_open = millis();
  if (!f.open(parent, file_name, O_RDONLY)) {
    _last_open_stats.file_open_ms = millis() - t_before_open;
    return false;
  }
  _last_open_stats.file_open_ms = millis() - t_before_open;

  const uint32_t t_before_size = millis();
  _cached_size = f.fileSize();
  _last_open_stats.file_size_ms = millis() - t_before_size;

  return true;
}

void AudioFile::close() {
  if (!f) return;
  StorageSdLockGuard sd_lock(500);
  if (!sd_lock) {
    LOGW("[AudioFile] close lock timeout");
    return;
  }
  f.close();
}

ssize_t AudioFile::read(void* dst, size_t bytes) {
  if (!f) return -1;

  StorageSdLockGuard sd_lock(500);
  if (!sd_lock) {
    LOGE("[AudioFile] 获取 SD 锁超时");
    return -1;
  }

  uint32_t current_pos = f.curPosition();

  if (current_pos >= _cached_size) {
    return 0;
  }

  uint32_t remaining = _cached_size - current_pos;
  if (remaining > bytes) {
    remaining = bytes;
  }

  int n = f.read(dst, remaining);

  if (n < 0) {
    return -1;
  } else if (n == 0 && remaining > 0) {
    LOGE("[AudioFile] 读取异常：期望 %u 字节但返回 0", remaining);
    return -1;
  }

  return n;
}

bool AudioFile::seek(uint32_t pos) {
  if (!f) {
    LOGE("[AudioFile] Seek 失败：文件未打开");
    return false;
  }

  StorageSdLockGuard sd_lock(500);
  if (!sd_lock) {
    LOGE("[AudioFile] 获取 SD 锁超时");
    return false;
  }

  if (pos > _cached_size) {
    LOGW("[AudioFile] Seek 超出范围：请求 %u，文件大小 %u", pos, _cached_size);
    pos = _cached_size;
  }

  bool result = f.seekSet(pos);

  if (!result) {
    LOGE("[AudioFile] Seek 失败：位置 %u，文件大小 %u", pos, _cached_size);
  }

  return result;
}

uint32_t AudioFile::tell() {
  return f ? (uint32_t)f.curPosition() : 0;
}

uint32_t AudioFile::size() {
  return f ? _cached_size : 0;
}
