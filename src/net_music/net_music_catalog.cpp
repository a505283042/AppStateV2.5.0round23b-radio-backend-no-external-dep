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
    LOGW("[NETMUSIC] base file not found: %s", kNetMusicBasePath);
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
    LOGW("[NETMUSIC] base url empty: %s", kNetMusicBasePath);
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
        item.album = trim_copy(line.substring(p4 + 1));
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
    LOGW("[NETMUSIC] line too long idx=%lu len=%u",
         (unsigned long)idx,
         (unsigned)line.length());
    return false;
  }

  if (!parse_line(line, out)) {
    s_error = "net_music_parse_failed";
    LOGW("[NETMUSIC] parse failed idx=%lu", (unsigned long)idx);
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
    LOGW("[NETMUSIC] catalog load skipped: SD lock failed");
    return false;
  }

  if (!read_base_url_locked()) {
    return false;
  }

  File32 f = sd.open(kNetMusicListPath, O_RDONLY);
  if (!f) {
    s_error = "net_music_list_missing";
    LOGW("[NETMUSIC] list not found: %s", kNetMusicListPath);
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

  LOGI("[NETMUSIC] catalog loaded tracks=%lu offsets=%u base=%s path=%s",
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