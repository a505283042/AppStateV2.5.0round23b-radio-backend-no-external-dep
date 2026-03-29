#include "meta/meta_id3.h"
#include "storage/storage_io.h"

static uint32_t read_u32_be(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint32_t read_syncsafe_u32(const uint8_t* p) {
  return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) |
         ((uint32_t)(p[2] & 0x7F) << 7)  | ((uint32_t)(p[3] & 0x7F));
}

static void utf8_append_codepoint(String& out, uint32_t cp) {
  if (cp <= 0x7F) {
    out += (char)cp;
  } else if (cp <= 0x7FF) {
    out += (char)(0xC0 | (cp >> 6));
    out += (char)(0x80 | (cp & 0x3F));
  } else if (cp <= 0xFFFF) {
    out += (char)(0xE0 | (cp >> 12));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  } else {
    out += (char)(0xF0 | (cp >> 18));
    out += (char)(0x80 | ((cp >> 12) & 0x3F));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  }
}

static String decode_text_frame(const uint8_t* buf, size_t len) {
  if (len == 0) return String();
  uint8_t enc = buf[0];
  const uint8_t* p = buf + 1;
  size_t n = len - 1;

  // 0: ISO-8859-1 (很多中文文件其实会塞 GBK，这里无法完美识别，先原样当 8bit)
  if (enc == 0) {
    String s;
    s.reserve(n);
    for (size_t i = 0; i < n; i++) {
      if (p[i] == 0) break;
      s += (char)p[i];
    }
    s.trim();
    return s;
  }

  // 3: UTF-8
  if (enc == 3) {
    String s((const char*)p, n);
    s.trim();
    return s;
  }

  // 1: UTF-16 with BOM, 2: UTF-16BE (no BOM)
  bool be = (enc == 2);
  size_t i = 0;

  if (enc == 1 && n >= 2) {
    // BOM
    if (p[0] == 0xFF && p[1] == 0xFE) { be = false; i = 2; }
    else if (p[0] == 0xFE && p[1] == 0xFF) { be = true; i = 2; }
  }

  String out;
  out.reserve(n); // 粗略
  for (; i + 1 < n; i += 2) {
    uint16_t u = be ? ((uint16_t)p[i] << 8 | p[i+1]) : ((uint16_t)p[i+1] << 8 | p[i]);
    if (u == 0x0000) break;
    utf8_append_codepoint(out, u);
    if (out.length() > 256) break; // 防止超长标签拖慢
  }
  out.trim();
  return out;
}

static bool read_id3v1(File32& f, Id3BasicInfo& out) {
  uint32_t sz = f.fileSize();
  if (sz < 128) return false;
  if (!f.seekSet(sz - 128)) return false;

  uint8_t tag[128];
  int n = f.read(tag, 128);
  if (n != 128) return false;
  if (!(tag[0]=='T' && tag[1]=='A' && tag[2]=='G')) return false;

  auto read_fixed = [](const uint8_t* p, size_t len)->String {
    String s;
    s.reserve(len);
    for (size_t i=0;i<len;i++){
      if (p[i]==0) break;
      s += (char)p[i];
    }
    s.trim();
    return s;
  };

  String title  = read_fixed(tag + 3, 30);
  String artist = read_fixed(tag + 33, 30);
  String album  = read_fixed(tag + 63, 30);

  if (out.title.length()==0)  out.title  = title;
  if (out.artist.length()==0) out.artist = artist;
  if (out.album.length()==0)  out.album  = album;
  return true;
}

bool id3_read_basic(SdFat& sd, const char* path, Id3BasicInfo& out)
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
  if (f.read(hdr, 10) != 10) {
    f.close();
    return false;
  }

  // ID3v2 header
  if (hdr[0]=='I' && hdr[1]=='D' && hdr[2]=='3') {
    uint8_t ver = hdr[3];          // 3 or 4 常见
    uint8_t flags = hdr[5];
    uint32_t tag_size = read_syncsafe_u32(hdr + 6); // 不含10字节头

    // 跳过扩展头(简单处理)
    uint32_t pos = 10;
    if (flags & 0x40) {
      // v2.3 扩展头 size 是 4 bytes big-endian；v2.4 是 syncsafe
      uint8_t ex[4];
      if (f.read(ex, 4) == 4) {
        uint32_t exsz = (ver == 4) ? read_syncsafe_u32(ex) : read_u32_be(ex);
        // ID3v2.3: exsz 包含 4 字节长度描述本身
        // ID3v2.4: exsz 不包含长度描述字节
        pos += (ver == 4) ? (4 + exsz) : exsz;
        f.seekSet(pos);
      }
    }

    uint32_t end = 10 + tag_size;
    // 安全上限：最多解析前 64KB 标签，避免异常文件卡死
    if (end > 10 + 64*1024) end = 10 + 64*1024;

    while (pos + 10 <= end) {
      uint8_t fh[10];
      if (f.read(fh, 10) != 10) break;

      // padding
      if (fh[0]==0 && fh[1]==0 && fh[2]==0 && fh[3]==0) break;

      char id[5] = { (char)fh[0], (char)fh[1], (char)fh[2], (char)fh[3], 0 };
      uint32_t fsz = (ver == 4) ? read_syncsafe_u32(fh + 4) : read_u32_be(fh + 4);
      pos += 10;

      if (fsz == 0 || pos + fsz > end) {
        // 跳过异常
        break;
      }

      // 只读我们关心的帧，且限制最大读入长度
      bool want = (!strcmp(id,"TIT2") || !strcmp(id,"TPE1") || !strcmp(id,"TALB"));
      if (want) {
        const size_t MAX_READ = 512;
        size_t toRead = (fsz > MAX_READ) ? MAX_READ : fsz;
        uint8_t buf[MAX_READ];
        int rn = f.read(buf, toRead);
        if (rn > 0) {
          String v = decode_text_frame(buf, (size_t)rn);
          if (!strcmp(id,"TIT2") && out.title.length()==0)  out.title  = v;
          if (!strcmp(id,"TPE1") && out.artist.length()==0) out.artist = v;
          if (!strcmp(id,"TALB") && out.album.length()==0)  out.album  = v;
        }
        // 跳过剩余部分
        if (fsz > toRead) f.seekCur((int32_t)(fsz - toRead));
      } else {
        f.seekCur((int32_t)fsz);
      }

      pos += fsz;

      if (out.title.length() && out.artist.length() && out.album.length()) break;
    }

    // 兜底 ID3v1
    read_id3v1(f, out);
    f.close();
    return true;
  }

  // 没有 ID3v2：尝试 ID3v1
  read_id3v1(f, out);
  f.close();
  return true;
}
