#include "web/web_cover_cache.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp32-hal-psram.h>
#include <string.h>
#include <stdlib.h>

#include "utils/log.h"

#undef LOG_TAG
#define LOG_TAG "WEBCOVER"

namespace {

struct WebCoverBmpSlot {
  bool valid = false;
  int track_idx = -1;
  uint32_t key = 0;
  uint8_t* bmp = nullptr;
  size_t bmp_len = 0;
  uint32_t last_touch_ms = 0;
};

static WebCoverBmpSlot s_slots[2];
static StaticSemaphore_t s_mu_buf;
static SemaphoreHandle_t s_mu = nullptr;

static SemaphoreHandle_t web_cover_cache_mutex() {
  if (!s_mu) {
    s_mu = xSemaphoreCreateMutexStatic(&s_mu_buf);
  }
  return s_mu;
}

static uint32_t fnv1a_add_bytes(uint32_t h, const char* s) {
  if (!s) return h;
  const uint8_t* p = reinterpret_cast<const uint8_t*>(s);
  while (*p) {
    h ^= *p++;
    h *= 16777619u;
  }
  return h;
}

static uint32_t fnv1a_add_u32(uint32_t h, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    h ^= (uint8_t)((v >> (i * 8)) & 0xFF);
    h *= 16777619u;
  }
  return h;
}

static uint32_t make_key(int track_idx,
                         CoverSource cover_source,
                         const char* audio_path,
                         const char* cover_path,
                         uint32_t cover_offset,
                         uint32_t cover_size) {
  uint32_t h = 2166136261u;
  h = fnv1a_add_u32(h, (uint32_t)track_idx);
  h = fnv1a_add_u32(h, (uint32_t)cover_source);
  h = fnv1a_add_u32(h, cover_offset);
  h = fnv1a_add_u32(h, cover_size);
  h = fnv1a_add_bytes(h, audio_path);
  h = fnv1a_add_bytes(h, cover_path);
  return h;
}

static void free_slot(WebCoverBmpSlot& s) {
  if (s.bmp) {
    free(s.bmp);
    s.bmp = nullptr;
  }
  s.valid = false;
  s.track_idx = -1;
  s.key = 0;
  s.bmp_len = 0;
  s.last_touch_ms = 0;
}

static int find_slot(int track_idx, uint32_t key) {
  for (int i = 0; i < 2; ++i) {
    if (s_slots[i].valid &&
        s_slots[i].track_idx == track_idx &&
        s_slots[i].key == key) {
      return i;
    }
  }
  return -1;
}

static int choose_store_slot(int track_idx) {
  const int exist = find_slot(track_idx, 0); // 不用 key 复用时别走这里
  (void)exist;

  for (int i = 0; i < 2; ++i) {
    if (!s_slots[i].valid) return i;
  }

  return (s_slots[0].last_touch_ms <= s_slots[1].last_touch_ms) ? 0 : 1;
}

static inline uint8_t c5to8(uint8_t v) { return (v << 3) | (v >> 2); }
static inline uint8_t c6to8(uint8_t v) { return (v << 2) | (v >> 4); }

static void le16(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void le32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
  p[2] = (uint8_t)((v >> 16) & 0xFF);
  p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static bool build_bmp24_from_sprite(LGFX_Sprite& spr, uint8_t** out_buf, size_t* out_len) {
  if (!out_buf || !out_len) return false;
  *out_buf = nullptr;
  *out_len = 0;

  const int w = spr.width();
  const int h = spr.height();
  if (w <= 0 || h <= 0) return false;

  const size_t row_stride = (size_t)(((w * 3) + 3) & ~3);
  const size_t pixel_bytes = row_stride * (size_t)h;
  const size_t file_size = 54 + pixel_bytes;

  uint8_t* buf = (uint8_t*)ps_malloc(file_size);
  if (!buf) buf = (uint8_t*)malloc(file_size);
  if (!buf) return false;

  memset(buf, 0, file_size);

  // BMP FILE HEADER
  buf[0] = 'B';
  buf[1] = 'M';
  le32(buf + 2, (uint32_t)file_size);
  le32(buf + 10, 54);

  // BMP INFO HEADER
  le32(buf + 14, 40);
  le32(buf + 18, (uint32_t)w);
  le32(buf + 22, (uint32_t)h);
  le16(buf + 26, 1);
  le16(buf + 28, 24);
  le32(buf + 34, (uint32_t)pixel_bytes);

  uint8_t* dst = buf + 54;

  for (int y = h - 1; y >= 0; --y) {
    uint8_t* row = dst + (size_t)(h - 1 - y) * row_stride;
    uint8_t* p = row;

    for (int x = 0; x < w; ++x) {
      const uint16_t c = spr.readPixel(x, y);
      const uint8_t r = c5to8((c >> 11) & 0x1F);
      const uint8_t g = c6to8((c >> 5) & 0x3F);
      const uint8_t b = c5to8(c & 0x1F);

      *p++ = b;
      *p++ = g;
      *p++ = r;
    }
  }

  *out_buf = buf;
  *out_len = file_size;
  return true;
}

} // namespace

bool web_cover_cache_has(int track_idx,
                         CoverSource cover_source,
                         const char* audio_path,
                         const char* cover_path,
                         uint32_t cover_offset,
                         uint32_t cover_size) {
  if (track_idx < 0) return false;

  SemaphoreHandle_t mu = web_cover_cache_mutex();
  if (!mu) return false;
  if (xSemaphoreTake(mu, pdMS_TO_TICKS(10)) != pdTRUE) return false;

  const uint32_t key = make_key(track_idx, cover_source, audio_path, cover_path, cover_offset, cover_size);
  const int slot = find_slot(track_idx, key);
  const bool ok = (slot >= 0);
  if (ok) s_slots[slot].last_touch_ms = millis();

  xSemaphoreGive(mu);
  return ok;
}

bool web_cover_cache_copy_bmp(int track_idx,
                              CoverSource cover_source,
                              const char* audio_path,
                              const char* cover_path,
                              uint32_t cover_offset,
                              uint32_t cover_size,
                              uint8_t** out_buf,
                              size_t* out_len) {
  if (!out_buf || !out_len) return false;
  *out_buf = nullptr;
  *out_len = 0;

  if (track_idx < 0) return false;

  SemaphoreHandle_t mu = web_cover_cache_mutex();
  if (!mu) return false;
  if (xSemaphoreTake(mu, pdMS_TO_TICKS(20)) != pdTRUE) return false;

  const uint32_t key = make_key(track_idx, cover_source, audio_path, cover_path, cover_offset, cover_size);
  const int slot = find_slot(track_idx, key);
  if (slot < 0 || !s_slots[slot].bmp || s_slots[slot].bmp_len == 0) {
    xSemaphoreGive(mu);
    return false;
  }

  uint8_t* copy = (uint8_t*)ps_malloc(s_slots[slot].bmp_len);
  if (!copy) copy = (uint8_t*)malloc(s_slots[slot].bmp_len);
  if (!copy) {
    xSemaphoreGive(mu);
    return false;
  }

  memcpy(copy, s_slots[slot].bmp, s_slots[slot].bmp_len);
  *out_buf = copy;
  *out_len = s_slots[slot].bmp_len;
  s_slots[slot].last_touch_ms = millis();

  xSemaphoreGive(mu);
  return true;
}

bool web_cover_cache_store_from_sprite(int track_idx,
                                       CoverSource cover_source,
                                       const char* audio_path,
                                       const char* cover_path,
                                       uint32_t cover_offset,
                                       uint32_t cover_size,
                                       LGFX_Sprite& spr) {
  if (track_idx < 0) return false;

  SemaphoreHandle_t mu0 = web_cover_cache_mutex();
  if (mu0 && xSemaphoreTake(mu0, pdMS_TO_TICKS(10)) == pdTRUE) {
    const uint32_t key = make_key(track_idx,
                                  cover_source,
                                  audio_path,
                                  cover_path,
                                  cover_offset,
                                  cover_size);
    const int old_slot = find_slot(track_idx, key);
    if (old_slot >= 0) {
      s_slots[old_slot].last_touch_ms = millis();
      xSemaphoreGive(mu0);
      LOGI("[WEBCOVER] skip ready track=%d", track_idx);
      return true;
    }
    xSemaphoreGive(mu0);
  }

  uint8_t* bmp = nullptr;
  size_t bmp_len = 0;
  if (!build_bmp24_from_sprite(spr, &bmp, &bmp_len) || !bmp || bmp_len == 0) {
    if (bmp) free(bmp);
    return false;
  }

  SemaphoreHandle_t mu = web_cover_cache_mutex();
  if (!mu) {
    free(bmp);
    return false;
  }
  if (xSemaphoreTake(mu, pdMS_TO_TICKS(50)) != pdTRUE) {
    free(bmp);
    return false;
  }

  const uint32_t key = make_key(track_idx, cover_source, audio_path, cover_path, cover_offset, cover_size);
  int slot = find_slot(track_idx, key);
  if (slot < 0) slot = choose_store_slot(track_idx);

  free_slot(s_slots[slot]);
  s_slots[slot].valid = true;
  s_slots[slot].track_idx = track_idx;
  s_slots[slot].key = key;
  s_slots[slot].bmp = bmp;
  s_slots[slot].bmp_len = bmp_len;
  s_slots[slot].last_touch_ms = millis();

  xSemaphoreGive(mu);

  LOGI("[WEBCOVER] ready track=%d bytes=%u", track_idx, (unsigned)bmp_len);
  return true;
}

void web_cover_cache_clear() {
  SemaphoreHandle_t mu = web_cover_cache_mutex();
  if (!mu) return;
  if (xSemaphoreTake(mu, pdMS_TO_TICKS(50)) != pdTRUE) return;

  for (int i = 0; i < 2; ++i) free_slot(s_slots[i]);

  xSemaphoreGive(mu);
}