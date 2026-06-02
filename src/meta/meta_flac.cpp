#include "meta/meta_flac.h"
#include "storage/storage_io.h"

static uint32_t read_u24_be(const uint8_t* p) {
  return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

static uint32_t read_u32_le(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool read_exact(File32& f, void* dst, size_t n) {
  return (size_t)f.read(dst, n) == n;
}

static bool seek_skip(File32& f, uint32_t n) {
  // SdFat 的 seekCur 用 int32；这里做分段跳过
  while (n > 0) {
    uint32_t step = (n > 0x7FFFFFFF) ? 0x7FFFFFFF : n;
    if (!f.seekCur((int32_t)step)) return false;
    n -= step;
  }
  return true;
}

static String to_upper_ascii(String s) {
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c >= 'a' && c <= 'z') s.setCharAt(i, c - 32);
  }
  return s;
}

static void apply_kv(FlacBasicInfo& out, const String& key, const String& val) {
  String k = to_upper_ascii(key);

  if (k == "TITLE")  { if (!out.title.length())  out.title  = val; return; }
  if (k == "ARTIST") { if (!out.artist.length()) out.artist = val; return; }
  if (k == "ALBUM")  { if (!out.album.length())  out.album  = val; return; }

  // 你后面想做聚合可扩展：
  // if (k == "ALBUMARTIST") ...
}

bool flac_read_vorbis_basic(SdFat& sd, const char* path, FlacBasicInfo& out)
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

  // 1) FLAC magic: "fLaC"
  uint8_t magic[4];
  if (!read_exact(f, magic, 4)) { 
    f.close(); 
    return false;
  }
  if (!(magic[0]=='f' && magic[1]=='L' && magic[2]=='a' && magic[3]=='C')) {
    f.close();
    return false;
  }

  // 2) 逐块读取 metadata blocks
  bool found_vc = false;

  for (int block_idx = 0; block_idx < 64; block_idx++) { // 安全上限：最多扫 64 个块
    uint8_t bh[4];
    if (!read_exact(f, bh, 4)) break;

    bool is_last = (bh[0] & 0x80) != 0;
    uint8_t type = (bh[0] & 0x7F);
    uint32_t len = read_u24_be(bh + 1);

    if (type == 4 /* VORBIS_COMMENT */) {
      found_vc = true;

      // Vorbis Comment 格式（全是 little-endian）：
      // [vendor_len u32][vendor_string][user_comment_list_len u32][comments...]
      uint8_t u32buf[4];

      if (!read_exact(f, u32buf, 4)) break;
      uint32_t vendor_len = read_u32_le(u32buf);
      if (vendor_len > len) break;
      if (!seek_skip(f, vendor_len)) break;

      if (!read_exact(f, u32buf, 4)) break;
      uint32_t n_comments = read_u32_le(u32buf);

      // 已消耗的字节：4 + vendor_len + 4
      uint32_t consumed = 8 + vendor_len;
      if (consumed > len) break;

      for (uint32_t i = 0; i < n_comments; i++) {
        if (!read_exact(f, u32buf, 4)) { consumed = len; break; }
        uint32_t c_len = read_u32_le(u32buf);
        consumed += 4;
        if (consumed + c_len > len) { // 防止越界
          seek_skip(f, (len > consumed) ? (len - consumed) : 0);
          consumed = len;
          break;
        }

        // 限制单条 comment 最大读取，避免超大字段占内存/耗时
        const uint32_t MAX_COMMENT = 1024;
        uint32_t to_read = (c_len > MAX_COMMENT) ? MAX_COMMENT : c_len;

        String line;
        line.reserve(to_read + 1);

        // 优化版：批量读取文本
        if (to_read > 0) {
            char* buf = (char*)malloc(to_read + 1);
            if (buf) {
                if (f.read(buf, to_read) == (int)to_read) {
                    buf[to_read] = '\0';
                    line = String(buf);
                }
                free(buf);
            }
        }
        // 跳过剩余
        if (c_len > to_read) seek_skip(f, c_len - to_read);

        consumed += c_len;

        // 解析 KEY=VALUE
        int eq = line.indexOf('=');
        if (eq > 0) {
          String key = line.substring(0, eq);
          String val = line.substring(eq + 1);
          val.trim();
          apply_kv(out, key, val);
        }

        // 如果我们要的三个字段都拿到，就可以提前结束（但仍需跳过块剩余）
        if (out.title.length() && out.artist.length() && out.album.length()) {
          break;
        }
      }

      // 跳过当前 block 剩余字节（如果没刚好读完）
      // 计算块内已读量比较麻烦，这里用保守方式：重新定位到 block 结束
      // 我们知道 block 起点在读完 bh 后的位置，无法回溯的话就“尽量读满”策略：
      // ——上面 consumed 已尽量追踪
      if (consumed < len) seek_skip(f, len - consumed);

      break; // 我们只需要第一个 Vorbis Comment
    } else {
      // 非 VC：跳过 block
      if (!seek_skip(f, len)) break;
    }

    if (is_last) break;
  }

  f.close();
  // 找到 FLAC 头并扫描完：返回 true；是否读到字段由 out.xxx 是否为空决定
  return found_vc;
}
