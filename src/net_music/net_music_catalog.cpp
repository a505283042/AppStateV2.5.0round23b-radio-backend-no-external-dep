#include "net_music/net_music_catalog.h"

#include <HTTPClient.h>
#include <SdFat.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <vector>

#include "storage/storage_io.h"
#include "utils/log.h"

extern SdFat sd;

namespace {

std::vector<uint32_t> s_offsets;
bool s_loaded = false;
bool s_base_loaded = false;
String s_error;
String s_base_url;

// NAS 歌曲列表只放内存，不写入 TF。
// s_offsets 记录每一行在 s_list_buf 里的起始位置。
char* s_list_buf = nullptr;
uint32_t s_list_len = 0;
uint32_t s_list_cap = 0;

constexpr const char* kNetMusicListName = "net_music.txt";
constexpr const char* kNetMusicMemoryPath = "memory:http/net_music.txt";
constexpr const char* kNetMusicBasePath = "/System/net_music_base.txt";
constexpr uint32_t kMaxNetMusicLineLen = 768;
constexpr uint32_t kHttpChunkSize = 256;
constexpr uint32_t kHttpIdleTimeoutMs = 8000;

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

static void free_remote_list_buffer() {
  if (s_list_buf) {
    heap_caps_free(s_list_buf);
  }

  s_list_buf = nullptr;
  s_list_len = 0;
  s_list_cap = 0;
}

static bool reserve_remote_list_capacity(uint32_t required_len) {
  // 多留 1 字节放 '\0'，方便调试和安全截断。
  const uint32_t required_cap = required_len + 1;
  if (required_cap <= s_list_cap) {
    return true;
  }

  uint32_t new_cap = s_list_cap ? s_list_cap : 4096;
  while (new_cap < required_cap) {
    new_cap *= 2;
  }

  void* p = heap_caps_realloc(s_list_buf,
                              new_cap,
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

  if (p) {
    LOGI("[网络音乐] 列表内存使用 PSRAM cap=%lu", (unsigned long)new_cap);
  } else {
    LOGW("[网络音乐] PSRAM 分配失败，尝试内部 RAM cap=%lu", (unsigned long)new_cap);
    p = heap_caps_realloc(s_list_buf, new_cap, MALLOC_CAP_8BIT);
  }

  if (!p) {
    s_error = "net_music_memory_alloc_failed";
    LOGE("[网络音乐] 列表内存分配失败 need=%lu cap=%lu",
         (unsigned long)required_len,
         (unsigned long)new_cap);
    return false;
  }

  s_list_buf = static_cast<char*>(p);
  s_list_cap = new_cap;
  return true;
}

static bool append_remote_list_bytes(const uint8_t* data, uint32_t len) {
  if (!data || len == 0) {
    return true;
  }

  if (!reserve_remote_list_capacity(s_list_len + len)) {
    return false;
  }

  memcpy(s_list_buf + s_list_len, data, len);
  s_list_len += len;
  s_list_buf[s_list_len] = '\0';
  return true;
}

static bool read_base_url_locked() {
  s_base_url = "";
  s_base_loaded = false;

  File32 f = sd.open(kNetMusicBasePath, O_RDONLY);
  if (!f) {
    s_error = "net_music_base_missing";
    LOGW("[网络音乐] base 文件未找到: %s", kNetMusicBasePath);
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

  s_base_loaded = true;
  s_error = "";

  LOGI("[网络音乐] base 加载成功: %s", s_base_url.c_str());
  return true;
}

static bool load_base_from_tf(uint32_t lock_timeout_ms) {
  StorageSdLockGuard guard(lock_timeout_ms);
  if (!guard) {
    s_error = "sd_lock_failed_base";
    LOGW("[网络音乐] 读取 base 失败：获取 SD 锁超时");
    return false;
  }

  return read_base_url_locked();
}

static bool ensure_base_loaded() {
  if (s_base_loaded && s_base_url.length()) {
    return true;
  }

  // 正常情况下开机已经读取过 base。
  // 这里是兜底：如果开机没读到，打开 NAS 时再读一次很小的 base 文件。
  return load_base_from_tf(800);
}

static String build_remote_list_url() {
  String url = s_base_url;
  if (!url.endsWith("/")) {
    url += "/";
  }
  url += kNetMusicListName;
  return url;
}

static bool download_remote_list_to_memory() {
  free_remote_list_buffer();

  if (!WiFi.isConnected()) {
    s_error = "wifi_not_connected";
    LOGW("[网络音乐] 下载列表失败：WiFi 未连接");
    return false;
  }

  const String url = build_remote_list_url();
  LOGI("[网络音乐] 下载列表到内存: %s", url.c_str());

  HTTPClient http;
  http.setTimeout(6000);
  http.setReuse(false);

  if (!http.begin(url)) {
    s_error = "http_begin_failed";
    LOGW("[网络音乐] HTTP begin 失败: %s", url.c_str());
    return false;
  }

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    s_error = "http_get_failed";
    LOGW("[网络音乐] 下载列表失败 HTTP=%d URL=%s", code, url.c_str());
    http.end();
    return false;
  }

  const int content_len = http.getSize();
  if (content_len > 0) {
    (void)reserve_remote_list_capacity((uint32_t)content_len);
  }

  WiFiClient* stream = http.getStreamPtr();
  uint8_t chunk[kHttpChunkSize];
  uint32_t last_data_ms = millis();

  while (http.connected()) {
    const int avail = stream ? stream->available() : 0;

    if (avail > 0) {
      const uint32_t want =
          (uint32_t)avail > kHttpChunkSize ? kHttpChunkSize : (uint32_t)avail;

      const int got = stream->readBytes(chunk, want);
      if (got > 0) {
        if (!append_remote_list_bytes(chunk, (uint32_t)got)) {
          http.end();
          free_remote_list_buffer();
          return false;
        }

        last_data_ms = millis();

        if (content_len > 0 && s_list_len >= (uint32_t)content_len) {
          break;
        }

        continue;
      }
    }

    if (content_len > 0 && s_list_len >= (uint32_t)content_len) {
      break;
    }

    if (millis() - last_data_ms > kHttpIdleTimeoutMs) {
      s_error = "http_idle_timeout";
      LOGW("[网络音乐] 下载列表超时 bytes=%lu URL=%s",
           (unsigned long)s_list_len,
           url.c_str());
      http.end();
      free_remote_list_buffer();
      return false;
    }

    // 网络暂时没数据时主动让出 CPU，避免 AudioTask / WiFi 轮询触发看门狗。
    delay(1);
  }

  http.end();

  if (s_list_len == 0 || !s_list_buf) {
    s_error = "net_music_list_empty";
    LOGW("[网络音乐] 下载到的列表为空: %s", url.c_str());
    free_remote_list_buffer();
    return false;
  }

  // 保险补 '\0'，后面不会依赖它遍历，但方便调试。
  if (reserve_remote_list_capacity(s_list_len)) {
    s_list_buf[s_list_len] = '\0';
  }

  LOGI("[网络音乐] 列表下载到内存完成 bytes=%lu",
       (unsigned long)s_list_len);

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

static bool copy_line_from_memory(uint32_t line_start, String& out) {
  out = "";

  if (!s_list_buf || line_start >= s_list_len) {
    s_error = "net_music_index_out_of_range";
    return false;
  }

  uint32_t pos = line_start;
  while (pos < s_list_len && s_list_buf[pos] != '\n') {
    ++pos;
  }

  uint32_t line_len = pos - line_start;

  // 去掉 Windows 文本里的 '\r'
  if (line_len > 0 && s_list_buf[line_start + line_len - 1] == '\r') {
    --line_len;
  }

  if (line_len > kMaxNetMusicLineLen) {
    s_error = "net_music_line_too_long";
    LOGW("[网络音乐] 行 too long offset=%lu len=%lu",
         (unsigned long)line_start,
         (unsigned long)line_len);
    return false;
  }

  out.reserve(line_len + 1);
  for (uint32_t i = 0; i < line_len; ++i) {
    out += s_list_buf[line_start + i];
  }

  return true;
}

static bool read_item_from_memory(uint32_t idx, NetMusicItem* out) {
  if (!out || idx >= s_offsets.size()) {
    s_error = "net_music_index_out_of_range";
    return false;
  }

  String line;
  if (!copy_line_from_memory(s_offsets[idx], line)) {
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

bool net_music_catalog_load_base() {
  return load_base_from_tf(800);
}

bool net_music_catalog_load() {
  s_offsets.clear();
  s_loaded = false;
  s_error = "";
  free_remote_list_buffer();

  // 打开 NAS 时不读 /System/net_music.txt，也不写 /System/net_music.txt。
  // 这里只使用开机已读入的 base；如果没有，再兜底读一次很小的 base 文件。
  if (!ensure_base_loaded()) {
    return false;
  }

  // 从 NAS 下载 net_music.txt 到内存。
  if (!download_remote_list_to_memory()) {
    s_loaded = false;
    return false;
  }

  uint32_t valid_count = 0;
  uint32_t pos = 0;

  while (pos < s_list_len) {
    const uint32_t line_start = pos;

    while (pos < s_list_len && s_list_buf[pos] != '\n') {
      ++pos;
    }

    String line;
    if (copy_line_from_memory(line_start, line)) {
      NetMusicItem probe{};
      if (parse_line(line, &probe)) {
        s_offsets.push_back(line_start);
        ++valid_count;
      }
    }

    if (pos < s_list_len && s_list_buf[pos] == '\n') {
      ++pos;
    }
  }

  if (valid_count == 0) {
    s_loaded = false;
    s_error = "net_music_no_valid_items";
    LOGW("[网络音乐] 远程列表没有有效 MP3 条目");
    free_remote_list_buffer();
    return false;
  }

  s_loaded = true;
  s_error = "";

  LOGI("[网络音乐] 目录 加载ed 歌曲s=%lu 偏移s=%u base=%s 来源=memory",
       (unsigned long)valid_count,
       (unsigned)s_offsets.size(),
       s_base_url.c_str());

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

  // NAS 列表在内存里，不再读 TF，不会和本地播放抢 SD 锁。
  return read_item_from_memory(idx, out);
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

  uint32_t matched_total = 0;

  for (uint32_t i = 0; i < s_offsets.size(); ++i) {
    String line;
    if (!copy_line_from_memory(s_offsets[i], line)) {
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
  return kNetMusicMemoryPath;
}

const char* net_music_catalog_base_path() {
  return kNetMusicBasePath;
}

void net_music_catalog_clear() {
  s_offsets.clear();
  s_loaded = false;
  s_base_loaded = false;
  s_error = "";
  s_base_url = "";
  free_remote_list_buffer();
}