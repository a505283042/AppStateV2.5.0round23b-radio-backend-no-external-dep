#include "ui/ui_cover_mem.h"
#include "esp32-hal-psram.h"
#include "utils/log.h"
#include "storage/storage_io.h"
#include <freertos/task.h>

static bool detect_png_from_buffer(const uint8_t* b, size_t len) {
  return (len >= 8 && b[0]==0x89 && b[1]=='P' && b[2]=='N' && b[3]=='G' && b[4]==0x0D && b[5]==0x0A && b[6]==0x1A && b[7]==0x0A);
}

static bool detect_jpg_from_buffer(const uint8_t* b, size_t len) {
  return (len >= 2 && b[0]==0xFF && b[1]==0xD8);
}

// 400KB 上限：够大多数封面，避免炸内存
static constexpr size_t MAX_COVER_BYTES_HARD = 400 * 1024;
static constexpr size_t COVER_READ_CHUNK = 8 * 1024;  // 分块读取，避免长时间霸占 SD 锁
static uint8_t* s_cover_buf = nullptr;
static bool s_cover_initialized = false;

// 初始化封面缓冲区（在系统启动时调用一次）
bool cover_init_buffer(void)
{
  if (s_cover_initialized) return true;

  uint32_t ps_before   = ESP.getFreePsram();
  uint32_t heap_before = ESP.getFreeHeap();

  uint8_t* p = (uint8_t*)ps_malloc(MAX_COVER_BYTES_HARD);
  if (!p) p = (uint8_t*)malloc(MAX_COVER_BYTES_HARD);
  if (!p) return false;

  uint32_t ps_after   = ESP.getFreePsram();
  uint32_t heap_after = ESP.getFreeHeap();

  LOGI("COVER", "init=%u ptr=%p psram_used=%u heap_used=%u free_psram=%u free_heap=%u",
       (unsigned)MAX_COVER_BYTES_HARD, p,
       (unsigned)(ps_before - ps_after),
       (unsigned)(heap_before - heap_after),
       (unsigned)ps_after,
       (unsigned)heap_after);

  s_cover_buf = p;
  s_cover_initialized = true;
  return true;
}

static bool ensure_cover_buf(size_t need)
{
  if (need == 0 || need > MAX_COVER_BYTES_HARD) return false;

  if (!s_cover_initialized) {
    LOGW("COVER", "buffer not initialized, call cover_init_buffer() first");
    return false;
  }

  return (s_cover_buf != nullptr);
}

bool cover_ensure_buffer(size_t need)
{
  return ensure_cover_buf(need);
}

uint8_t* cover_get_buffer()
{
  return s_cover_buf;
}

static bool open_file_locked(SdFat& sd, const char* path, File32& f)
{
  StorageSdLockGuard lock(1000);
  if (!lock) {
    LOGW("COVER", "open lock timeout: %s", path ? path : "<null>");
    return false;
  }

  f = sd.open(path, O_RDONLY);
  return (bool)f;
}

static bool close_file_locked(File32& f)
{
  if (!f) return true;
  StorageSdLockGuard lock(1000);
  if (!lock) {
    LOGW("COVER", "close lock timeout");
    return false;
  }
  f.close();
  return true;
}

static bool read_open_file_chunked_to_buffer(File32& f, uint8_t* dst, uint32_t offset, size_t size, bool& out_is_png)
{
  if (!dst || size == 0) return false;

  {
    StorageSdLockGuard lock(1000);
    if (!lock) {
      LOGW("COVER", "seek lock timeout");
      return false;
    }
    if (!f.seekSet(offset)) {
      return false;
    }
  }

  size_t copied = 0;
  while (copied < size) {
    const size_t chunk = (size - copied > COVER_READ_CHUNK) ? COVER_READ_CHUNK : (size - copied);

    StorageSdLockGuard lock(1000);
    if (!lock) {
      LOGW("COVER", "read lock timeout @%u", (unsigned)copied);
      return false;
    }

    const int r = f.read(dst + copied, chunk);
    if (r != (int)chunk) {
      LOGW("COVER", "read failed want=%u got=%d", (unsigned)chunk, r);
      return false;
    }
    copied += chunk;

    if (copied < size) {
      vTaskDelay(1);
    }
  }

  out_is_png = detect_png_from_buffer(dst, size);
  if (!out_is_png && !detect_jpg_from_buffer(dst, size)) {
    LOGW("COVER", "unknown image header, fallback to JPEG decode path");
  }
  return true;
}

static bool read_open_file_chunked(File32& f, uint32_t offset, size_t size, bool& out_is_png)
{
  if (!ensure_cover_buf(size)) return false;

  {
    StorageSdLockGuard lock(1000);
    if (!lock) {
      LOGW("COVER", "seek lock timeout");
      return false;
    }
    if (!f.seekSet(offset)) {
      return false;
    }
  }

  size_t copied = 0;
  while (copied < size) {
    const size_t chunk = (size - copied > COVER_READ_CHUNK) ? COVER_READ_CHUNK : (size - copied);

    StorageSdLockGuard lock(1000);
    if (!lock) {
      LOGW("COVER", "read lock timeout @%u", (unsigned)copied);
      return false;
    }

    const int r = f.read(s_cover_buf + copied, chunk);
    if (r != (int)chunk) {
      LOGW("COVER", "read failed want=%u got=%d", (unsigned)chunk, r);
      return false;
    }
    copied += chunk;

    if (copied < size) {
      vTaskDelay(1);
    }
  }

  out_is_png = detect_png_from_buffer(s_cover_buf, size);
  if (!out_is_png && !detect_jpg_from_buffer(s_cover_buf, size)) {
    LOGW("COVER", "unknown image header, fallback to JPEG decode path");
  }
  return true;
}

bool cover_load_to_memory(SdFat& sd, const TrackInfo& t, const uint8_t*& out_ptr, size_t& out_len, bool& out_is_png)
{
  out_ptr = nullptr;
  out_len = 0;
  out_is_png = false;

  File32 f;
  uint32_t size = 0;
  uint32_t offset = 0;
  String path;

  if (t.cover_source == COVER_FILE_FALLBACK && t.cover_path.length()) {
    path = t.cover_path;

    if (!open_file_locked(sd, path.c_str(), f)) return false;
    {
      StorageSdLockGuard lock(1000);
      if (!lock) {
        close_file_locked(f);
        return false;
      }
      size = f.fileSize();
    }
    offset = 0;
  } else if ((t.cover_source == COVER_MP3_APIC || t.cover_source == COVER_FLAC_PICTURE) && t.cover_size > 0) {
    path = t.audio_path;
    size = t.cover_size;
    offset = t.cover_offset;

    if (!open_file_locked(sd, path.c_str(), f)) return false;
  } else {
    return false;
  }

  if (size == 0 || !ensure_cover_buf(size)) {
    close_file_locked(f);
    return false;
  }

  const bool ok = read_open_file_chunked(f, offset, size, out_is_png);
  close_file_locked(f);
  if (!ok) return false;

  out_ptr = s_cover_buf;
  out_len = size;
  return true;
}


bool cover_load_alloc(SdFat& sd, const TrackInfo& t, uint8_t*& out_buf, size_t& out_len, bool& out_is_png)
{
  out_buf = nullptr;
  out_len = 0;
  out_is_png = false;

  File32 f;
  uint32_t size = 0;
  uint32_t offset = 0;
  String path;

  if (t.cover_source == COVER_FILE_FALLBACK && t.cover_path.length()) {
    path = t.cover_path;
    if (!open_file_locked(sd, path.c_str(), f)) return false;
    {
      StorageSdLockGuard lock(1000);
      if (!lock) {
        close_file_locked(f);
        return false;
      }
      size = f.fileSize();
    }
    offset = 0;
  } else if ((t.cover_source == COVER_MP3_APIC || t.cover_source == COVER_FLAC_PICTURE) && t.cover_size > 0) {
    path = t.audio_path;
    size = t.cover_size;
    offset = t.cover_offset;
    if (!open_file_locked(sd, path.c_str(), f)) return false;
  } else {
    return false;
  }

  if (size == 0 || size > MAX_COVER_BYTES_HARD) {
    close_file_locked(f);
    return false;
  }

  uint8_t* buf = (uint8_t*)ps_malloc(size);
  if (!buf) buf = (uint8_t*)malloc(size);
  if (!buf) {
    close_file_locked(f);
    return false;
  }

  const bool ok = read_open_file_chunked_to_buffer(f, buf, offset, size, out_is_png);
  close_file_locked(f);
  if (!ok) {
    free(buf);
    return false;
  }

  out_buf = buf;
  out_len = size;
  return true;
}

void cover_free_alloc(uint8_t* p)
{
  if (p) free(p);
}
