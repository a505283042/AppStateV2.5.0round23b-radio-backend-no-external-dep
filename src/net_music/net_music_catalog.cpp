#include "net_music/net_music_catalog.h"

#include <SdFat.h>
#include <vector>

#include "storage/storage_io.h"
#include "utils/log.h"

extern SdFat sd;

namespace {

std::vector<uint32_t> s_offsets;
bool s_loaded = false;
String s_error;
String s_base_url;

constexpr const char* kNetMusicListPath = "/System/net_music.txt";
constexpr const char* kNetMusicBasePath = "/System/net_music_base.txt";
constexpr uint32_t kMaxNetMusicLineLen = 768;

static String trim_copy(const String& in) {
  String s = in;
  s.trim();
  return s;
}

static void strip_utf8_bom(String& s) {
  if (s.length() >= 3 &&
      (uint8_t)s[0] == 0xEF &&
      (uint8_t)s[1] == 0xBB &&
      (uint8_t)s[2] == 0xBF) {
    s.remove(0, 3);
  }
}

static bool read_base_url_locked() {
  s_base_url = "";

  File32 f = sd.open(kNetMusicBasePath, O_RDONLY);
  if (!f) {
    s_error = "net_music_base_missing";
    LOGW("[网络音乐] base 文件 未找到: %s", kNetMusicBasePath);
    return false;
  }

  while (f.available()) {
    String line = f.readStringUntil('\n');
    strip_utf8_bom(line);
    line = trim_copy(line);

    if (!line.length()) continue;
    if (line.startsWith("#") || line.startsWith(";")) continue;

    s_base_url = line;
    break;
  }

  f.close();

  if (!s_base_url.length()) {
    s_error = "net_music_base_empty";
    LOGW("[网络音乐] base URL 为空: %s", kNetMusicBasePath);
    return false;
  }

  if (!s_base_url.endsWith("/")) {
    s_base_url += "/";
  }

  return true;
}

static bool parse_line(const String& raw, NetMusicItem* out) {
  if (!out) return false;

  String line = trim_copy(raw);
  strip_utf8_bom(line);
  line = trim_copy(line);

  if (!line.length()) return false;
  if (line.startsWith("#") || line.startsWith(";")) return false;

  const int p1 = line.indexOf('|');
  if (p1 <= 0) return false;

  const int p2 = line.indexOf('|', p1 + 1);
  const int p3 = p2 >= 0 ? line.indexOf('|', p2 + 1) : -1;
  const int p4 = p3 >= 0 ? line.indexOf('|', p3 + 1) : -1;
  const int p5 = p4 >= 0 ? line.indexOf('|', p4 + 1) : -1;

  NetMusicItem item{};
  item.title = trim_copy(line.substring(0, p1));

  if (p2 < 0) {
    item.encoded_path = trim_copy(line.substring(p1 + 1));
  } else {
    item.encoded_path = trim_copy(line.substring(p1 + 1, p2));

    if (p3 < 0) {
      item.format = trim_copy(line.substring(p2 + 1));
    } else {
      item.format = trim_copy(line.substring(p2 + 1, p3));

      if (p4 < 0) {
        item.artist = trim_copy(line.substring(p3 + 1));
      } else {
        item.artist = trim_copy(line.substring(p3 + 1, p4));

        if (p5 < 0) {
          item.album = trim_copy(line.substring(p4 + 1));
        } else {
          item.album = trim_copy(line.substring(p4 + 1, p5));
          String duration = trim_copy(line.substring(p5 + 1));
          item.duration_ms = (uint32_t)duration.toInt();
        }
      }
    }
  }

  if (!item.format.length()) {
    item.format = "mp3";
  }

  item.format.toLowerCase();

  if (!item.artist.length()) {
    item.artist = "NAS";
  }

  if (!item.album.length()) {
    item.album = "NAS";
  }

  item.valid =
      item.title.length() > 0 &&
      item.encoded_path.length() > 0 &&
      item.format == "mp3";

  *out = item;
  return item.valid;
}

static bool read_item_locked(uint32_t idx, NetMusicItem* out) {
  if (!out || idx >= s_offsets.size()) {
    s_error = "net_music_index_out_of_range";
    return false;
  }

  File32 f = sd.open(kNetMusicListPath, O_RDONLY);
  if (!f) {
    s_error = "net_music_list_open_failed";
    return false;
  }

  if (!f.seek(s_offsets[idx])) {
    f.close();
    s_error = "net_music_seek_failed";
    return false;
  }

  String line = f.readStringUntil('\n');
  f.close();

  if (line.length() > kMaxNetMusicLineLen) {
    s_error = "net_music_line_too_long";
    LOGW("[网络音乐] 行 too long idx=%lu le数量=%u",
         (unsigned long)idx,
         (unsigned)line.length());
    return false;
  }

  if (!parse_line(line, out)) {
    s_error = "net_music_parse_failed";
    LOGW("[网络音乐] parse 失败 idx=%lu", (unsigned long)idx);
    return false;
  }

  return true;
}

}  // namespace

bool net_music_catalog_load() {
  s_offsets.clear();
  s_loaded = false;
  s_error = "";

  StorageSdLockGuard guard(1500);
  if (!guard) {
    s_error = "sd_lock_failed";
    LOGW("[网络音乐] 跳过目录加载：获取 SD 锁失败");
    return false;
  }

  if (!read_base_url_locked()) {
    return false;
  }

  File32 f = sd.open(kNetMusicListPath, O_RDONLY);
  if (!f) {
    s_error = "net_music_list_missing";
    LOGW("[网络音乐] 列表文件未找到：%s", kNetMusicListPath);
    return false;
  }

  uint32_t valid_count = 0;

  while (f.available()) {
    const uint32_t line_start = (uint32_t)f.position();
    String line = f.readStringUntil('\n');

    NetMusicItem probe{};
    if (line.length() <= kMaxNetMusicLineLen && parse_line(line, &probe)) {
      s_offsets.push_back(line_start);
      ++valid_count;
    }
  }

  f.close();

  s_loaded = true;

  LOGI("[网络音乐] 目录 加载ed 歌曲s=%lu 偏移s=%u base=%s 路径=%s",
       (unsigned long)valid_count,
       (unsigned)s_offsets.size(),
       s_base_url.c_str(),
       kNetMusicListPath);

  return true;
}

bool net_music_catalog_is_loaded() {
  return s_loaded;
}

uint32_t net_music_catalog_count() {
  return (uint32_t)s_offsets.size();
}

bool net_music_catalog_get(uint32_t idx, NetMusicItem* out) {
  if (!s_loaded || !out) {
    s_error = "net_music_not_loaded";
    return false;
  }

  StorageSdLockGuard guard(1200);
  if (!guard) {
    s_error = "sd_lock_failed";
    return false;
  }

  return read_item_locked(idx, out);
}

uint32_t net_music_catalog_search(const String& query,
                                  uint16_t limit,
                                  std::vector<NetMusicSearchHit>* out) {
  if (out) {
    out->clear();
  }

  if (!s_loaded) {
    s_error = "net_music_not_loaded";
    return 0;
  }

  String q = query;
  q.trim();
  q.toLowerCase();

  if (!q.length()) {
    s_error = "net_music_search_empty";
    return 0;
  }

  if (limit == 0) {
    limit = 20;
  }

  if (limit > 50) {
    limit = 50;
  }

  StorageSdLockGuard guard(1800);
  if (!guard) {
    s_error = "sd_lock_failed";
    return 0;
  }

  File32 f = sd.open(kNetMusicListPath, O_RDONLY);
  if (!f) {
    s_error = "net_music_list_open_failed";
    return 0;
  }

  uint32_t matched_total = 0;

  for (uint32_t i = 0; i < s_offsets.size(); ++i) {
    if (!f.seek(s_offsets[i])) {
      continue;
    }

    String line = f.readStringUntil('\n');

    if (line.length() > kMaxNetMusicLineLen) {
      continue;
    }

    NetMusicItem item{};
    if (!parse_line(line, &item) || !item.valid) {
      continue;
    }

    String haystack;
    haystack.reserve(item.title.length() + item.artist.length() + item.album.length() + 4);
    haystack += item.title;
    haystack += " ";
    haystack += item.artist;
    haystack += " ";
    haystack += item.album;
    haystack.toLowerCase();

    if (haystack.indexOf(q) < 0) {
      continue;
    }

    ++matched_total;

    if (out && out->size() < limit) {
      NetMusicSearchHit hit{};
      hit.idx = i;
      hit.item = item;
      out->push_back(hit);
    }
  }

  f.close();

  LOGD("[网络音乐] search q=%s matched=%lu 返回数量=%u",
       q.c_str(),
       (unsigned long)matched_total,
       out ? (unsigned)out->size() : 0);

  return matched_total;
}

String net_music_catalog_build_url(const NetMusicItem& item) {
  if (!item.valid || !item.encoded_path.length()) {
    return String();
  }

  return s_base_url + item.encoded_path;
}

String net_music_catalog_base_url() {
  return s_base_url;
}

String net_music_catalog_error() {
  return s_error;
}

const char* net_music_catalog_path() {
  return kNetMusicListPath;
}

const char* net_music_catalog_base_path() {
  return kNetMusicBasePath;
}

void net_music_catalog_clear() {
  s_offsets.clear();
  s_loaded = false;
  s_error = "";
  s_base_url = "";
}