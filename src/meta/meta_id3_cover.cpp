#include "meta/meta_id3_cover.h"
#include "storage/storage_io.h"

static uint32_t read_u32_be(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint32_t read_syncsafe_u32(const uint8_t* p) {
  return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) |
         ((uint32_t)(p[2] & 0x7F) << 7)  | ((uint32_t)(p[3] & 0x7F));
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

// 读到 0 结束的 C 字符串（最多 maxLen）
static String read_cstr(File32& f, uint32_t maxLen, uint32_t& consumed) {
  String s;
  s.reserve(32);
  for (uint32_t i = 0; i < maxLen; i++) {
    int ch = f.read();
    if (ch < 0) break;
    consumed++;
    if (ch == 0) break;
    s += (char)ch;
  }
  return s;
}

// 跳过“description”字段（取决于 encoding）
static bool skip_description(File32& f, uint8_t enc, uint32_t remain, uint32_t& consumed) {
  if (remain == 0) return true;

  if (enc == 0 || enc == 3) {
    // ISO-8859-1 or UTF-8: 以 0 终止
    while (consumed < remain) {
      int ch = f.read();
      if (ch < 0) return false;
      consumed++;
      if (ch == 0) break;
    }
    return true;
  }

  // UTF-16/UTF-16BE: 以 0x00 0x00 终止（按 2 字节）
  while (consumed + 1 < remain) {
    int b0 = f.read();
    int b1 = f.read();
    if (b0 < 0 || b1 < 0) return false;
    consumed += 2;
    if (b0 == 0 && b1 == 0) break;
  }
  return true;
}

bool id3_find_apic(SdFat& sd, const char* path, Mp3CoverLoc& out)
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

  uint8_t hdr[10];
  if (!read_exact(f, hdr, 10)) { 
    f.close(); 
    return false;
  }
  if (!(hdr[0]=='I' && hdr[1]=='D' && hdr[2]=='3')) { 
    f.close(); 
    return true;
  }

  uint8_t ver = hdr[3]; // 3 or 4
  uint8_t flags = hdr[5];
  uint32_t tag_size = read_syncsafe_u32(hdr + 6);
  uint32_t pos = 10;
  uint32_t end = 10 + tag_size;

  // 扩展头简单跳过
  if (flags & 0x40) {
    uint8_t ex[4];
    if (read_exact(f, ex, 4)) {
      uint32_t exsz = (ver == 4) ? read_syncsafe_u32(ex) : read_u32_be(ex);
      // ID3v2.3: exsz 包含 4 字节长度描述本身
      // ID3v2.4: exsz 不包含长度描述字节
      pos += (ver == 4) ? (4 + exsz) : exsz;
      f.seekSet(pos);
    }
  }

  // 安全上限：最多解析 128KB 标签
  if (end > 10 + 128*1024) end = 10 + 128*1024;

  while (pos + 10 <= end) {
    uint8_t fh[10];
    if (!read_exact(f, fh, 10)) break;
    if (fh[0]==0 && fh[1]==0 && fh[2]==0 && fh[3]==0) break;

    char id[5] = { (char)fh[0], (char)fh[1], (char)fh[2], (char)fh[3], 0 };
    uint32_t fsz = (ver == 4) ? read_syncsafe_u32(fh + 4) : read_u32_be(fh + 4);

    pos += 10;
    if (fsz == 0 || pos + fsz > end) break;

    if (strcmp(id, "APIC") != 0) {
      skip_bytes(f, fsz);
      pos += fsz;
      continue;
    }

    // 检查帧标志位，跳过压缩或加密的 APIC 帧
    uint8_t flag1 = fh[8];
    uint8_t flag2 = fh[9];
    if (ver == 4) {
      // ID3v2.4: 检查压缩和加密标志
      if ((flag1 & 0x40) || (flag1 & 0x08)) {
        // 压缩或加密的帧，跳过
        skip_bytes(f, fsz);
        pos += fsz;
        continue;
      }
    }

    // ---- 解析 APIC 帧内部 ----
    uint32_t frame_start = pos; // 帧内容起点（不含10字节帧头）
    uint8_t enc = 0;
    if (!read_exact(f, &enc, 1)) break;

    uint32_t consumed = 1;
    uint32_t remain = fsz;

    // mime（0 terminated）
    String mime = read_cstr(f, remain - consumed, consumed);
    // picture type
    uint8_t pic_type = 0;
    if (!read_exact(f, &pic_type, 1)) break;
    consumed += 1;

    // description（跳过）
    if (!skip_description(f, enc, remain - consumed, consumed)) break;

    // image data 起点：直接使用当前文件指针位置
    uint32_t img_off = (uint32_t)f.position();
    uint32_t img_sz  = (fsz > consumed) ? (fsz - consumed) : 0;

    out.found = (img_sz > 0);
    out.offset = img_off;
    out.size = img_sz;
    out.mime = mime.length() ? mime : String();

    f.close();
    return true;
  }

  f.close();
  return true;
}
