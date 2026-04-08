#include <Arduino.h>
#include "ui/ui_internal.h"
#include "ui/ui_cover_mem.h"
#include "audio/audio.h"
#include "audio/audio_service.h"
#include "storage/storage_io.h"
#include "utils/log.h"
#include "web/web_cover_cache.h"
#undef LOG_TAG
#define LOG_TAG "UI"

#include <math.h>
#include <SdFat.h>
#include <esp32-hal-psram.h>

extern SdFat sd;

static bool png_get_wh(const uint8_t* data, size_t len, int& w, int& h);

// 从 JPEG 数据中获取图像宽度和高度
// 参数: data - JPEG 数据指针, len - 数据长度, w/h - 输出宽度和高度
// 返回: 成功返回 true，失败返回 false
static bool jpg_get_wh(const uint8_t* data, size_t len, int& w, int& h);

static bool png_get_wh(const uint8_t* data, size_t len, int& w, int& h)
{
  // 检查数据有效性
  if (!data || len < 24) return false;
  
  // 验证 PNG 签名 (8 字节魔数)
  const uint8_t sig[8] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
  for (int i = 0; i < 8; ++i) if (data[i] != sig[i]) return false;

  // 从 IHDR 块中提取宽度和高度 (大端序，各占 4 字节)
  w = (int)((uint32_t)data[16] << 24 | (uint32_t)data[17] << 16 | (uint32_t)data[18] << 8 | (uint32_t)data[19]);
  h = (int)((uint32_t)data[20] << 24 | (uint32_t)data[21] << 16 | (uint32_t)data[22] << 8 | (uint32_t)data[23]);
  
  // 验证尺寸有效性
  return (w > 0 && h > 0);
}

static bool jpg_get_wh(const uint8_t* data, size_t len, int& w, int& h)
{
  // 检查数据有效性
  if (!data || len < 4) return false;

  // 验证 JPEG SOI (Start of Image) 标记
  if (data[0] != 0xFF || data[1] != 0xD8) return false; // SOI

  size_t i = 2;

  // 判断是否为 SOF (Start of Frame) 标记
  auto is_sof = [](uint8_t m) {
    return (m==0xC0||m==0xC1||m==0xC2||m==0xC3||m==0xC5||m==0xC6||m==0xC7||
            m==0xC9||m==0xCA||m==0xCB||m==0xCD||m==0xCE||m==0xCF);
  };

  // 遍历 JPEG 段寻找 SOF 标记
  while (i + 3 < len) {
    // 跳过非 0xFF 字节
    if (data[i] != 0xFF) { ++i; continue; }

    // 跳过填充的 0xFF 字节
    while (i < len && data[i] == 0xFF) ++i;
    if (i >= len) break;

    // 读取标记字节
    uint8_t marker = data[i++];

    // 遇到 SOS (Start of Scan) 或 EOI (End of Image) 则停止
    if (marker == 0xDA || marker == 0xD9) break; // SOS / EOI

    // 读取段长度 (大端序，2 字节)
    if (i + 1 >= len) break;
    uint16_t seglen = (uint16_t)((data[i] << 8) | data[i + 1]);
    if (seglen < 2) return false;
    if (i + seglen > len) return false;

    // 找到 SOF 标记，提取图像尺寸
    if (is_sof(marker)) {
      size_t p = i + 2;
      if (p + 4 >= len) return false;
      // SOF 中高度在前，宽度在后 (各占 2 字节)
      h = (int)((data[p + 1] << 8) | data[p + 2]);
      w = (int)((data[p + 3] << 8) | data[p + 4]);
      // 验证尺寸有效性
      return (w > 0 && h > 0);
    }

    // 跳过当前段
    i += seglen;
  }
  return false;
}

static inline uint8_t _c5to8(uint8_t v) { return (v << 3) | (v >> 2); }
static inline uint8_t _c6to8(uint8_t v) { return (v << 2) | (v >> 4); }
static inline uint16_t _rgb888_to_565(uint8_t r, uint8_t g, uint8_t b)
{
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// crop=true：居中裁切填满（推荐）
// crop=false：等比缩放留黑边
static void cover_downscale_bilinear(LGFX_Sprite& src, LGFX_Sprite& dst, int dstSize, bool crop)
{
  int sw = src.width();
  int sh = src.height();
  if (sw <= 0 || sh <= 0) return;

  float sx = (float)dstSize / (float)sw;
  float sy = (float)dstSize / (float)sh;
  float s  = crop ? (sx > sy ? sx : sy) : (sx < sy ? sx : sy);

  float regionW = (float)dstSize / s;
  float regionH = (float)dstSize / s;
  float startX  = ((float)sw - regionW) * 0.5f;
  float startY  = ((float)sh - regionH) * 0.5f;

  float stepX = regionW / (float)dstSize;
  float stepY = regionH / (float)dstSize;

  dst.fillScreen(TFT_BLACK);

  for (int y = 0; y < dstSize; ++y) {
    float fy = startY + (y + 0.5f) * stepY - 0.5f;
    int y0 = (int)floorf(fy);
    float ty = fy - y0;
    if (y0 < 0) { y0 = 0; ty = 0; }
    int y1 = y0 + 1; if (y1 >= sh) y1 = sh - 1;

    for (int x = 0; x < dstSize; ++x) {
      float fx = startX + (x + 0.5f) * stepX - 0.5f;
      int x0 = (int)floorf(fx);
      float tx = fx - x0;
      if (x0 < 0) { x0 = 0; tx = 0; }
      int x1 = x0 + 1; if (x1 >= sw) x1 = sw - 1;

      uint16_t c00 = src.readPixel(x0, y0);
      uint16_t c10 = src.readPixel(x1, y0);
      uint16_t c01 = src.readPixel(x0, y1);
      uint16_t c11 = src.readPixel(x1, y1);

      // RGB565 -> RGB888
      uint8_t r00 = _c5to8((c00 >> 11) & 0x1F), g00 = _c6to8((c00 >> 5) & 0x3F), b00 = _c5to8(c00 & 0x1F);
      uint8_t r10 = _c5to8((c10 >> 11) & 0x1F), g10 = _c6to8((c10 >> 5) & 0x3F), b10 = _c5to8(c10 & 0x1F);
      uint8_t r01 = _c5to8((c01 >> 11) & 0x1F), g01 = _c6to8((c01 >> 5) & 0x3F), b01 = _c5to8(c01 & 0x1F);
      uint8_t r11 = _c5to8((c11 >> 11) & 0x1F), g11 = _c6to8((c11 >> 5) & 0x3F), b11 = _c5to8(c11 & 0x1F);

      // bilinear
      float w00 = (1 - tx) * (1 - ty);
      float w10 = (tx)     * (1 - ty);
      float w01 = (1 - tx) * (ty);
      float w11 = (tx)     * (ty);

      uint8_t r = (uint8_t)(r00*w00 + r10*w10 + r01*w01 + r11*w11 + 0.5f);
      uint8_t g = (uint8_t)(g00*w00 + g10*w10 + g01*w01 + g11*w11 + 0.5f);
      uint8_t b = (uint8_t)(b00*w00 + b10*w10 + b01*w01 + b11*w11 + 0.5f);

      dst.drawPixel(x, y, _rgb888_to_565(r, g, b));
    }
  }
}

// 将封面图像缩放并渲染到目标精灵对（原始 + 带遮罩）中
static bool cover_blit_scaled_to_pair(const uint8_t* ptr, size_t len, bool is_png,
                                      LGFX_Sprite& dst_cover, LGFX_Sprite& dst_masked)
{
  int srcW = 0, srcH = 0;
  bool got = is_png ? png_get_wh(ptr, len, srcW, srcH) : jpg_get_wh(ptr, len, srcW, srcH);
  if (!got || srcW <= 0 || srcH <= 0) {
    dst_cover.fillScreen(TFT_BLACK);
    bool ok = false;
    if (is_png) ok = dst_cover.drawPng(ptr, len, 0, 0, COVER_SIZE, COVER_SIZE);
    else        ok = dst_cover.drawJpg(ptr, len, 0, 0, COVER_SIZE, COVER_SIZE);
    if (ok) {
      dst_masked.fillScreen(TFT_BLACK);
      dst_cover.pushSprite(&dst_masked, 0, 0);
      dst_masked.fillRectAlpha(0, 0, COVER_SIZE, COVER_SIZE, 128, TFT_BLACK);
    }
    return ok;
  }

  if (srcW > 800 || srcH > 800) {
    dst_cover.fillScreen(TFT_BLACK);
    bool ok = is_png ? dst_cover.drawPng(ptr, len, 0, 0, COVER_SIZE, COVER_SIZE)
                     : dst_cover.drawJpg(ptr, len, 0, 0, COVER_SIZE, COVER_SIZE);
    if (ok) {
      dst_masked.fillScreen(TFT_BLACK);
      dst_cover.pushSprite(&dst_masked, 0, 0);
      dst_masked.fillRectAlpha(0, 0, COVER_SIZE, COVER_SIZE, 128, TFT_BLACK);
    }
    return ok;
  }

  LGFX_Sprite tmp(&tft);
  tmp.setColorDepth(16);
  tmp.setPsram(psramFound());

  if (!tmp.createSprite(srcW, srcH)) {
    LOGW("[COVER] tmp.createSprite(%d,%d) failed -> fallback crop", srcW, srcH);
    dst_cover.fillScreen(TFT_BLACK);
    bool ok = is_png ? dst_cover.drawPng(ptr, len, 0, 0, COVER_SIZE, COVER_SIZE)
                    : dst_cover.drawJpg(ptr, len, 0, 0, COVER_SIZE, COVER_SIZE);
    if (ok) {
      dst_masked.fillScreen(TFT_BLACK);
      dst_cover.pushSprite(&dst_masked, 0, 0);
      dst_masked.fillRectAlpha(0, 0, COVER_SIZE, COVER_SIZE, 128, TFT_BLACK);
    }
    return ok;
  }

  tmp.fillScreen(TFT_BLACK);
  bool ok = false;
  if (is_png) ok = tmp.drawPng(ptr, len, 0, 0);
  else        ok = tmp.drawJpg(ptr, len, 0, 0);

  if (!ok) {
    tmp.deleteSprite();
    return false;
  }

  dst_cover.fillScreen(TFT_BLACK);
  cover_downscale_bilinear(tmp, dst_cover, COVER_SIZE, true);

  dst_masked.fillScreen(TFT_BLACK);
  dst_cover.pushSprite(&dst_masked, 0, 0);
  dst_masked.fillRectAlpha(0, 0, COVER_SIZE, COVER_SIZE, 128, TFT_BLACK);

  tmp.deleteSprite();
  return true;
}

// 将封面图像缩放并渲染到当前显示用的 240x240 精灵中
static bool cover_blit_scaled_to_240(const uint8_t* ptr, size_t len, bool is_png)
{
  return cover_blit_scaled_to_pair(ptr, len, is_png, s_coverSpr, s_coverMasked);
}

// 前向声明
void cover_sprite_init_once();
void cover_cache_sprite_init_once();

static void ui_arm_rotate_start_after_audio_if_needed()
{
  if (s_view == UI_VIEW_ROTATE) {
    const uint32_t now_ms = millis();
    s_rot_last_ms = now_ms;
    s_angle_deg = 0.0f;
    s_rotate_wait_audio_start = !(audio_service_is_playing() && audio_get_play_ms() > 0);
  } else {
    s_rotate_wait_audio_start = false;
    s_rotate_wait_prefetch_done = false;
  }
}

static int ui_cover_cache_find_slot(int track_idx)
{
  if (track_idx < 0) return -1;
  for (int i = 0; i < 2; ++i) {
    if (s_coverCacheReady[i] && s_coverCacheTrackIdx[i] == track_idx) return i;
  }
  return -1;
}

static int ui_cover_cache_choose_store_slot(int track_idx)
{
  const int existing = ui_cover_cache_find_slot(track_idx);
  if (existing >= 0) return existing;

  for (int i = 0; i < 2; ++i) {
    if (!s_coverCacheReady[i]) return i;
  }

  const int current_track_idx = s_ui_track_idx;
  const int current_slot = ui_cover_cache_find_slot(current_track_idx);
  if (current_slot >= 0) {
    return (current_slot == 0) ? 1 : 0;
  }

  return 1;
}

static void cover_draw_placeholder(const char* msg)
{
  cover_sprite_init_once();
  s_coverSpr.fillScreen(TFT_BLACK);
  s_coverSpr.drawRect(0, 0, COVER_SIZE, COVER_SIZE, 0x7BEF);  // 亮灰边框

  s_coverSpr.setTextSize(2);
  s_coverSpr.setTextColor(TFT_WHITE);

  const char* s = (msg && msg[0]) ? msg : "NO COVER";
  int16_t x = (COVER_SIZE - s_coverSpr.textWidth(s)) / 2;
  if (x < 0) x = 0;
  s_coverSpr.setCursor(x, 110);
  s_coverSpr.print(s);
}

// 初始化封面精灵（仅执行一次）
// 功能: 创建 s_coverSpr（原始）和 s_coverMasked（带遮罩）两个精灵
//      如果检测到 PSRAM，则使用 PSRAM 以节省内部 RAM
void cover_sprite_init_once()
{
  if (s_coverSprInited) return;

  const bool has_psram = psramFound();

  s_coverSpr.setColorDepth(16);
  s_coverSpr.setPsram(has_psram);
  if (!s_coverSpr.createSprite(COVER_SIZE, COVER_SIZE)) {
    LOGW("[COVER] s_coverSpr.createSprite(%d,%d) failed", COVER_SIZE, COVER_SIZE);
    return;
  }
  s_coverSpr.fillScreen(TFT_BLACK);

  s_coverMasked.setColorDepth(16);
  s_coverMasked.setPsram(has_psram);
  if (!s_coverMasked.createSprite(COVER_SIZE, COVER_SIZE)) {
    LOGW("[COVER] s_coverMasked.createSprite(%d,%d) failed", COVER_SIZE, COVER_SIZE);
    s_coverSpr.deleteSprite();
    return;
  }
  s_coverMasked.fillScreen(TFT_BLACK);

  s_coverSprInited = true;
}

void cover_cache_sprite_init_once()
{
  if (s_coverCacheInited) return;

  const bool has_psram = psramFound();
  for (int i = 0; i < 2; ++i) {
    s_coverCacheSpr[i]->setColorDepth(16);
    s_coverCacheSpr[i]->setPsram(has_psram);
    if (!s_coverCacheSpr[i]->createSprite(COVER_SIZE, COVER_SIZE)) {
      LOGW("[COVER] s_coverCacheSpr[%d].createSprite(%d,%d) failed", i, COVER_SIZE, COVER_SIZE);
      for (int j = 0; j < i; ++j) s_coverCacheSpr[j]->deleteSprite();
      return;
    }
    s_coverCacheSpr[i]->fillScreen(TFT_BLACK);

    s_coverCacheMasked[i]->setColorDepth(16);
    s_coverCacheMasked[i]->setPsram(has_psram);
    if (!s_coverCacheMasked[i]->createSprite(COVER_SIZE, COVER_SIZE)) {
      LOGW("[COVER] s_coverCacheMasked[%d].createSprite(%d,%d) failed", i, COVER_SIZE, COVER_SIZE);
      s_coverCacheSpr[i]->deleteSprite();
      for (int j = 0; j < i; ++j) {
        s_coverCacheMasked[j]->deleteSprite();
        s_coverCacheSpr[j]->deleteSprite();
      }
      return;
    }
    s_coverCacheMasked[i]->fillScreen(TFT_BLACK);
    s_coverCacheReady[i] = false;
    s_coverCacheTrackIdx[i] = -1;
  }

  s_coverCacheInited = true;
}

// 初始化双缓冲帧精灵（仅执行一次）
// 功能: 创建 INFO 视图和 ROTATE 视图的双缓冲帧
//      如果检测到 PSRAM，则使用 PSRAM 以节省内部 RAM
void cover_frames_init_once()
{
  // 如果已经初始化过，直接返回
  if (s_framesInited) return;

  const bool has_psram = psramFound();

  // INFO 视图双缓冲帧
  for (int i = 0; i < 2; ++i) {
    s_frame[i]->setColorDepth(16);
    s_frame[i]->setPsram(has_psram);
    if (!s_frame[i]->createSprite(COVER_SIZE, COVER_SIZE)) {
      LOGW("[COVER] s_frame[%d].createSprite(%d,%d) failed", i, COVER_SIZE, COVER_SIZE);
      // 清理已创建的精灵
      for (int j = 0; j < i; ++j) {
        s_frame[j]->deleteSprite();
      }
      return;
    }
    s_frame[i]->fillScreen(TFT_BLACK);
  }
  s_front = 0;
  s_back  = 1;
  s_framesInited = true;

  // ROTATE 视图双缓冲帧
  for (int i = 0; i < 2; ++i) {
    s_rotFrame[i]->setColorDepth(16);
    s_rotFrame[i]->setPsram(has_psram);
    if (!s_rotFrame[i]->createSprite(COVER_SIZE, COVER_SIZE)) {
      LOGW("[COVER] s_rotFrame[%d].createSprite(%d,%d) failed", i, COVER_SIZE, COVER_SIZE);
      // 清理已创建的精灵
      for (int j = 0; j < i; ++j) {
        s_rotFrame[j]->deleteSprite();
      }
      return;
    }
    s_rotFrame[i]->fillScreen(TFT_BLACK);
  }
  s_rotFront = 0;
  s_rotBack  = 1;
  s_rotFramesInited = true;
}

// 设置封面旋转动画的源精灵
// 参数: src - 源精灵指针（通常是 s_coverSpr）
// 功能: 指定要旋转的封面精灵，后续的旋转操作将基于此精灵

bool cover_decode_to_sprite_from_track(const TrackInfo& t)
{
  uint32_t t0 = millis();
  
  // 初始化封面精灵
  cover_sprite_init_once();
  uint32_t t1 = millis();

  const uint8_t* ptr = nullptr;
  size_t len = 0;
  bool is_png = false;

  // 尝试从曲目文件中加载封面到内存
  if (!cover_load_to_memory(sd, t, ptr, len, is_png)) {

  // 默认封面缺失 “画占位并当作成功”
    {
      StorageSdLockGuard sd_lock(1000);
      if (!sd_lock || !sd.exists("/System/default_cover.jpg")) {
        LOGW("[COVER] default cover not found -> placeholder");
        ui_lock();
        cover_draw_placeholder("NO COVER");
        s_coverSprReady = true;
        ui_unlock();
        return true;
      }
    }

    File32 f;
    {
      StorageSdLockGuard sd_lock(1000);
      if (!sd_lock) {
        LOGW("[COVER] open default cover lock timeout");
        s_coverSprReady = false;
        return false;
      }
      f = sd.open("/System/default_cover.jpg", O_RDONLY);
    }
    if (!f) {
      LOGW("[COVER] open default cover failed");
      s_coverSprReady = false;
      return false;
    }

    uint32_t fileSize = 0;
    {
      StorageSdLockGuard sd_lock(1000);
      if (!sd_lock) {
        f.close();
        s_coverSprReady = false;
        return false;
      }
      fileSize = f.fileSize();
    }
    if (fileSize == 0 || !cover_ensure_buffer(fileSize)) {
      StorageSdLockGuard sd_lock(1000);
      if (sd_lock) f.close();
      LOGW("[COVER] ensure buffer failed for default cover");
      s_coverSprReady = false;
      return false;
    }

    uint8_t* buf = cover_get_buffer();
    int bytesRead = 0;
    {
      StorageSdLockGuard sd_lock(1000);
      if (!sd_lock) {
        f.close();
        s_coverSprReady = false;
        return false;
      }
      bytesRead = f.read(buf, fileSize);
      f.close();
    }
    if (bytesRead != (int)fileSize) {
      LOGW("[COVER] read default cover failed");
      s_coverSprReady = false;
      return false;
    }

    // 将默认封面缩放到 240x240
    ui_lock();
    bool ok = cover_blit_scaled_to_240(buf, fileSize, false);

    s_coverSprReady = ok;
    LOGI("[COVER] default cover ok=%d", (int)ok);
    ui_unlock();
    return ok;
  }

  uint32_t t2 = millis();
  
  // 使用 MIME 类型判断图像格式（比猜测更可靠）
  if (t.cover_mime == "image/png") is_png = true;
  else if (t.cover_mime == "image/jpeg") is_png = false;

  // 将封面图像缩放到 240x240
  ui_lock();
  uint32_t t3 = millis();
  bool ok = cover_blit_scaled_to_240(ptr, len, is_png);
  uint32_t t4 = millis();

  s_coverSprReady = ok;
  LOGI("[COVER] decode: init=%lums, load=%lums, scale=%lums, total=%lums", 
       t1-t0, t2-t1, t4-t3, t4-t0);
  ui_unlock();
  return ok;
}

// 封面数据缓存（用于拆分加载）
static const uint8_t* s_cover_mem_ptr = nullptr;
static size_t s_cover_mem_len = 0;
static bool s_cover_mem_is_png = false;

static bool ui_cover_scale_common(const uint8_t* ptr, size_t len, bool is_png)
{
  if (!ptr || len == 0) {
    return false;
  }

  ui_lock();
  bool ok = cover_blit_scaled_to_240(ptr, len, is_png);
  s_coverSprReady = ok;
  if (ok) {
    s_src = &s_coverSpr;
    ui_arm_rotate_start_after_audio_if_needed();
  }
  ui_unlock();
  return ok;
}

// 从 SD 读取封面到内存（SD 卡操作）
bool ui_cover_load_to_memory(const TrackInfo& t)
{
  s_cover_mem_ptr = nullptr;
  s_cover_mem_len = 0;
  s_cover_mem_is_png = false;

  cover_sprite_init_once();

  const uint8_t* ptr = nullptr;
  size_t len = 0;
  bool is_png = false;

  if (!cover_load_to_memory(sd, t, ptr, len, is_png)) {
    return false;
  }

  s_cover_mem_ptr = ptr;
  s_cover_mem_len = len;
  s_cover_mem_is_png = is_png;
  return true;
}

// 从内存解码缩放到精灵（不访问 SD）
bool ui_cover_scale_from_memory()
{
  const bool ok = ui_cover_scale_common(s_cover_mem_ptr, s_cover_mem_len, s_cover_mem_is_png);
  s_cover_mem_ptr = nullptr;
  s_cover_mem_len = 0;
  return ok;
}

bool ui_cover_scale_from_buffer(const uint8_t* ptr, size_t len, bool is_png)
{
  return ui_cover_scale_common(ptr, len, is_png);
}

bool ui_cover_scale_to_cache_from_buffer(const uint8_t* ptr, size_t len, bool is_png, int track_idx)
{
  if (!ptr || len == 0 || track_idx < 0) return false;
  cover_cache_sprite_init_once();
  if (!s_coverCacheInited) return false;

  const int slot = ui_cover_cache_choose_store_slot(track_idx);
  if (slot < 0 || slot > 1) return false;

  bool ok = false;

  ui_lock();
  ok = cover_blit_scaled_to_pair(ptr, len, is_png, *s_coverCacheSpr[slot], *s_coverCacheMasked[slot]);
  if (ok) {
    s_coverCacheReady[slot] = true;
    s_coverCacheTrackIdx[slot] = track_idx;
  }
  ui_unlock();

  return ok;
}

bool ui_cover_apply_cached(int track_idx)
{
  const int slot = ui_cover_cache_find_slot(track_idx);
  if (slot < 0) return false;
  if (!s_coverSprInited || !s_coverCacheInited) return false;

  ui_lock();
  s_coverCacheSpr[slot]->pushSprite(&s_coverSpr, 0, 0);
  s_coverCacheMasked[slot]->pushSprite(&s_coverMasked, 0, 0);
  s_coverSprReady = true;
  s_src = &s_coverSpr;
  s_cover_apply_ms = millis();
  s_rotate_release_ms = 0;
  s_rotate_release_audio_ms = 0;
  s_rotate_probe_frames_left = 0;
  ui_arm_rotate_start_after_audio_if_needed();
  ui_unlock();
  return true;
}

bool ui_cover_cache_is_ready(int track_idx)
{
  return ui_cover_cache_find_slot(track_idx) >= 0;
}

void ui_cover_cache_invalidate()
{
  for (int i = 0; i < 2; ++i) {
    s_coverCacheReady[i] = false;
    s_coverCacheTrackIdx[i] = -1;
  }
  web_cover_cache_clear();
}

bool ui_cover_load_allocated(const TrackInfo& t, uint8_t*& out_buf, size_t& out_len, bool& out_is_png)
{
  return cover_load_alloc(sd, t, out_buf, out_len, out_is_png);
}

void ui_cover_free_allocated(uint8_t* p)
{
  cover_free_alloc(p);
}
