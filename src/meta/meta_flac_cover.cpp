#include "meta/meta_flac_cover.h"
#include "storage/storage_io.h"

static uint32_t read_u24_be(const uint8_t* p) {
  return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}
static uint32_t read_u32_be(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static bool read_exact(File32& f, void* dst, size_t n) {
  return (size_t)f.read(dst, n) == n;
}
static bool skip_bytes(File32& f, uint32_t n) {
  while (n > 0) {
    uint32_t step = (n > 0x7FFFFFFF) ? 0x7FFFFFFF : n;
    if (!f.seekCur((int32_t)step)) return false;
    n -= step;
  }
  return true;
}

static bool read_u32_be_file(File32& f, uint32_t& v)
{
  uint8_t b[4];
  if (f.read(b, 4) != 4) return false;
  v = ((uint32_t)b[0] << 24) |
      ((uint32_t)b[1] << 16) |
      ((uint32_t)b[2] << 8)  |
      (uint32_t)b[3];
  return true;
}

bool flac_find_picture(SdFat& sd, const char* path, FlacCoverLoc& out)
{
  out = {};

  StorageSdLockGuard sd_lock(1000);
  if (!sd_lock) {
    return false;
  }

  File32 f = sd.open(path, O_RDONLY);
  if (!f) {
    return false;
  }

  uint8_t magic[4];
  if (!read_exact(f, magic, 4)) {
    f.close();
    return false;
  }

  if (!(magic[0] == 'f' && magic[1] == 'L' && magic[2] == 'a' && magic[3] == 'C')) {
    f.close();
    return true;
  }

  // 记录一个“非 front cover”的候选图，最后兜底使用
  FlacCoverLoc fallback = {};
  bool has_fallback = false;

  while (true) {
    uint8_t bh[4];
    if (!read_exact(f, bh, 4)) break;

    bool is_last = (bh[0] & 0x80) != 0;
    uint8_t type = (bh[0] & 0x7F);
    uint32_t len = read_u24_be(bh + 1);

    if (type != 6 /* PICTURE */) {
      if (!skip_bytes(f, len)) break;
      if (is_last) break;
      continue;
    }

    uint64_t block_start = f.position();

    uint32_t picture_type = 0;
    uint32_t mime_len = 0;
    uint32_t desc_len = 0;
    uint32_t w = 0, h = 0, depth = 0, colors = 0;
    uint32_t data_len = 0;

    if (!read_u32_be_file(f, picture_type)) break;
    if (!read_u32_be_file(f, mime_len)) break;

    String mime;
    if (mime_len > 0) {
      if (mime_len <= 127) {
        char tmp[128];
        if (f.read((uint8_t*)tmp, mime_len) != (int)mime_len) break;
        tmp[mime_len] = 0;
        mime = String(tmp);
      } else {
        // mime 太长，先跳过，但不算致命
        if (!skip_bytes(f, mime_len)) break;
      }
    }

    if (!read_u32_be_file(f, desc_len)) break;
    if (desc_len > 0) {
      if (!skip_bytes(f, desc_len)) break;
    }

    if (!read_u32_be_file(f, w)) break;
    if (!read_u32_be_file(f, h)) break;
    if (!read_u32_be_file(f, depth)) break;
    if (!read_u32_be_file(f, colors)) break;
    if (!read_u32_be_file(f, data_len)) break;

    uint64_t data_off = f.position();

    // 优先 Front Cover(type=3)，但保留第一个可用图片作为兜底
    if (data_len > 0) {
      if (picture_type == 3) {
        out.found = true;
        out.offset = data_off;
        out.size = data_len;
        out.mime = mime;

        f.close();
        return true;
      }

      if (!has_fallback) {
        fallback.found = true;
        fallback.offset = data_off;
        fallback.size = data_len;
        fallback.mime = mime;
        has_fallback = true;
      }
    }

    // 跳到这个 PICTURE block 末尾
    uint64_t consumed = f.position() - block_start;
    if (consumed > len) break;

    uint32_t remain = (uint32_t)(len - consumed);
    if (remain > 0) {
      if (!skip_bytes(f, remain)) break;
    }

    if (is_last) break;
  }

  if (has_fallback) {
    out = fallback;
  }

  f.close();
  return true;
}
