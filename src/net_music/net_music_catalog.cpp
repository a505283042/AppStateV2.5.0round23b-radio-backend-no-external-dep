#include "net_music/net_music_catalog.h"

#include <HTTPClient.h>
#include <Preferences.h>
#include <SdFat.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include "storage/storage_io.h"
#include "storage/system_paths.h"
#include "utils/log.h"
#include "utils/text_normalize.h"

extern SdFat sd;

namespace {

uint32_t* s_offsets = nullptr;
uint32_t s_offsets_count = 0;
uint32_t s_offsets_cap = 0;
bool s_loaded = false;
bool s_base_loaded = false;
String s_error;
String s_base_url;

constexpr uint8_t kMaxNetMusicSources = 8;
NetMusicSourceInfo s_sources[kMaxNetMusicSources];
uint8_t s_source_count = 0;
uint8_t s_active_source_idx = 0;

// NAS 歌曲列表只放内存，不写入 TF。
// s_offsets 记录每一行在 s_list_buf 里的起始位置，强制放到 PSRAM，降低 WiFi 开启后的内部 RAM 压力。
char* s_list_buf = nullptr;
uint32_t s_list_len = 0;
uint32_t s_list_cap = 0;

constexpr const char* kNetMusicListName = "net_music.txt";
constexpr const char* kNetMusicMemoryPath = "memory:http/net_music.txt";
constexpr const char* kNetMusicBasePath = SystemPaths::kNetMusicBase;
constexpr const char* kNetMusicSourcesPath = SystemPaths::kNetMusicSources;
constexpr const char* kNetMusicPrefsNs = "netmusic";
constexpr const char* kNetMusicSourcePrefKey = "source";
constexpr uint32_t kMaxNetMusicLineLen = 768;
constexpr uint32_t kNetMusicMaxItems = UINT16_MAX;
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

static bool is_url_hex_digit(char c) {
  return (c >= '0' && c <= '9') ||
         (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}

static bool is_url_unreserved(uint8_t c) {
  return (c >= 'a' && c <= 'z') ||
         (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') ||
         c == '-' || c == '.' || c == '_' || c == '~';
}

static char url_hex_upper(uint8_t nibble) {
  nibble &= 0x0F;
  return nibble < 10
      ? static_cast<char>('0' + nibble)
      : static_cast<char>('A' + (nibble - 10));
}

// net_music.txt 第二列允许直接保存 UTF-8 原始路径。
// 构建 HTTP URL 时才逐字节编码；已有的 %XX 路径保持原样，兼容旧列表。
static size_t net_music_relative_path_start(const String& path) {
  size_t start = 0;

  while (start + 1 < path.length() &&
         path[start] == '.' &&
         (path[start + 1] == '/' || path[start + 1] == '\\')) {
    start += 2;
  }

  while (start < path.length() &&
         (path[start] == '/' || path[start] == '\\')) {
    ++start;
  }

  return start;
}

static size_t encoded_net_music_path_length(const String& path,
                                            size_t start) {
  size_t encoded_len = 0;

  for (size_t i = start; i < path.length(); ++i) {
    const uint8_t c = static_cast<uint8_t>(path[i]);

    if (c == '/' || c == '\\' || is_url_unreserved(c)) {
      ++encoded_len;
      continue;
    }

    if (c == '%' &&
        i + 2 < path.length() &&
        is_url_hex_digit(path[i + 1]) &&
        is_url_hex_digit(path[i + 2])) {
      encoded_len += 3;
      i += 2;
      continue;
    }

    encoded_len += 3;
  }

  return encoded_len;
}

static void append_encoded_net_music_path(String& url,
                                          const String& path,
                                          size_t start) {
  for (size_t i = start; i < path.length(); ++i) {
    const uint8_t c = static_cast<uint8_t>(path[i]);

    if (c == '/' || c == '\\') {
      url += '/';
      continue;
    }

    if (is_url_unreserved(c)) {
      url += static_cast<char>(c);
      continue;
    }

    // 旧列表已经写成 %E4%B8%AD 时不要再次编码成 %25E4...。
    if (c == '%' &&
        i + 2 < path.length() &&
        is_url_hex_digit(path[i + 1]) &&
        is_url_hex_digit(path[i + 2])) {
      url += '%';
      url += path[i + 1];
      url += path[i + 2];
      i += 2;
      continue;
    }

    url += '%';
    url += url_hex_upper(c >> 4);
    url += url_hex_upper(c);
  }
}

static String normalize_source_path(const String& raw) {
  String path = trim_copy(raw);
  path.replace('\\', '/');

  while (path.startsWith("./")) {
    path.remove(0, 2);
  }
  while (path.startsWith("/")) {
    path.remove(0, 1);
  }
  while (path.endsWith("/")) {
    path.remove(path.length() - 1);
  }

  return path;
}

static void clear_source_config() {
  for (uint8_t i = 0; i < kMaxNetMusicSources; ++i) {
    s_sources[i] = NetMusicSourceInfo{};
  }
  s_source_count = 0;
  s_active_source_idx = 0;
}

static void install_default_source() {
  clear_source_config();
  s_sources[0].name = "NAS音乐";
  s_sources[0].relative_path = "";
  s_sources[0].list_name = kNetMusicListName;
  s_sources[0].valid = true;
  s_source_count = 1;
}

static bool parse_source_line(const String& raw, NetMusicSourceInfo& out) {
  String line = trim_copy(raw);
  strip_utf8_bom(line);
  line = trim_copy(line);

  if (!line.length() || line.startsWith("#") || line.startsWith(";")) {
    return false;
  }

  const int p1 = line.indexOf('|');
  const int p2 = p1 >= 0 ? line.indexOf('|', p1 + 1) : -1;

  NetMusicSourceInfo source{};
  if (p1 < 0) {
    source.name = line;
    source.relative_path = line;
  } else {
    source.name = trim_copy(line.substring(0, p1));
    source.relative_path = p2 < 0
        ? trim_copy(line.substring(p1 + 1))
        : trim_copy(line.substring(p1 + 1, p2));
    if (p2 >= 0) {
      source.list_name = trim_copy(line.substring(p2 + 1));
    }
  }

  source.relative_path = normalize_source_path(source.relative_path);
  source.list_name = normalize_source_path(source.list_name);

  if (!source.name.length()) {
    source.name = source.relative_path.length()
        ? source.relative_path
        : String("NAS音乐");
  }
  if (!source.list_name.length()) {
    source.list_name = kNetMusicListName;
  }

  // 防止配置越出 Web 音乐根目录。
  if (source.relative_path == ".." ||
      source.relative_path.startsWith("../") ||
      source.relative_path.indexOf("/../") >= 0 ||
      source.list_name == ".." ||
      source.list_name.startsWith("../") ||
      source.list_name.indexOf("/../") >= 0) {
    return false;
  }

  source.valid = true;
  out = source;
  return true;
}

static bool read_sources_locked() {
  clear_source_config();

  File32 f = sd.open(kNetMusicSourcesPath, O_RDONLY);
  if (!f) {
    install_default_source();
    LOGW("[网络音乐] 曲库源配置未找到，使用旧版单列表模式: %s",
         kNetMusicSourcesPath);
    return true;
  }

  while (f.available() && s_source_count < kMaxNetMusicSources) {
    String line = f.readStringUntil('\n');
    NetMusicSourceInfo source{};
    if (!parse_source_line(line, source)) {
      continue;
    }

    s_sources[s_source_count++] = source;
  }
  f.close();

  if (s_source_count == 0) {
    install_default_source();
    LOGW("[网络音乐] 曲库源配置没有有效条目，使用旧版单列表模式");
    return true;
  }

  LOGI("[网络音乐] 曲库源配置加载成功：数量=%u",
       (unsigned)s_source_count);
  return true;
}

static void restore_active_source_index() {
  Preferences pref;
  if (!pref.begin(kNetMusicPrefsNs, true)) {
    s_active_source_idx = 0;
    return;
  }

  const uint8_t saved = pref.getUChar(kNetMusicSourcePrefKey, 0);
  pref.end();
  s_active_source_idx = saved < s_source_count ? saved : 0;
}

static bool persist_active_source_index() {
  Preferences pref;
  if (!pref.begin(kNetMusicPrefsNs, false)) {
    return false;
  }

  const size_t written = pref.putUChar(kNetMusicSourcePrefKey, s_active_source_idx);
  pref.end();
  return written > 0;
}

static const NetMusicSourceInfo* active_source() {
  if (s_source_count == 0 || s_active_source_idx >= s_source_count) {
    return nullptr;
  }

  const NetMusicSourceInfo& source = s_sources[s_active_source_idx];
  return source.valid ? &source : nullptr;
}

static String build_active_source_root_url() {
  String url = s_base_url;
  if (!url.endsWith("/")) {
    url += "/";
  }

  const NetMusicSourceInfo* source = active_source();
  if (!source || !source->relative_path.length()) {
    return url;
  }

  const size_t start = net_music_relative_path_start(source->relative_path);
  const size_t encoded_len =
      encoded_net_music_path_length(source->relative_path, start);
  (void)url.reserve(url.length() + encoded_len + 2);
  append_encoded_net_music_path(url, source->relative_path, start);
  if (!url.endsWith("/")) {
    url += "/";
  }
  return url;
}


static void free_remote_offsets() {
  if (s_offsets) {
    heap_caps_free(s_offsets);
  }

  s_offsets = nullptr;
  s_offsets_count = 0;
  s_offsets_cap = 0;
}

static bool reserve_remote_offsets_capacity(uint32_t required_count) {
  if (required_count <= s_offsets_cap) {
    return true;
  }

  uint32_t new_cap = s_offsets_cap ? s_offsets_cap : 256;
  while (new_cap < required_count) {
    new_cap *= 2;
  }

  const size_t bytes = (size_t)new_cap * sizeof(uint32_t);
  void* p = heap_caps_realloc(s_offsets, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) {
    s_error = "net_music_offsets_psram_alloc_failed";
    LOGE("[网络音乐] 偏移表 PSRAM 分配失败 need=%lu cap=%lu bytes=%u",
         (unsigned long)required_count,
         (unsigned long)new_cap,
         (unsigned)bytes);
    return false;
  }

  s_offsets = static_cast<uint32_t*>(p);
  s_offsets_cap = new_cap;
  return true;
}

static bool append_remote_offset(uint32_t offset) {
  if (!reserve_remote_offsets_capacity(s_offsets_count + 1)) {
    return false;
  }

  s_offsets[s_offsets_count++] = offset;
  return true;
}

static void free_remote_list_buffer() {
  if (s_list_buf) {
    heap_caps_free(s_list_buf);
  }

  s_list_buf = nullptr;
  s_list_len = 0;
  s_list_cap = 0;
}

static void clear_loaded_list_state() {
  free_remote_offsets();
  free_remote_list_buffer();
  s_loaded = false;
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
  }

  if (!p) {
    LOGW("[网络音乐] 列表 PSRAM 分配失败，禁止回落内部RAM cap=%lu",
         (unsigned long)new_cap);
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

static void shrink_remote_list_capacity_to_len(uint32_t len) {
  if (!s_list_buf) {
    return;
  }

  const uint32_t required_cap = len + 1;
  if (required_cap >= s_list_cap) {
    return;
  }

  void* p = heap_caps_realloc(s_list_buf,
                              required_cap,
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

  if (p) {
    s_list_buf = static_cast<char*>(p);
    s_list_cap = required_cap;
    s_list_buf[s_list_len] = '\0';
  }
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
  bool ok = false;
  {
    StorageSdLockGuard guard(lock_timeout_ms);
    if (!guard) {
      s_error = "sd_lock_failed_base";
      LOGW("[网络音乐] 读取 base 失败：获取 SD 锁超时");
      return false;
    }

    ok = read_base_url_locked();
    if (ok) {
      ok = read_sources_locked();
    }
  }

  if (ok) {
    restore_active_source_index();
    const NetMusicSourceInfo* source = active_source();
    LOGI("[网络音乐] 当前曲库源：索引=%u 名称=%s 路径=%s",
         (unsigned)s_active_source_idx,
         source ? source->name.c_str() : "NAS音乐",
         source ? source->relative_path.c_str() : "");
  }
  return ok;
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
  String url = build_active_source_root_url();
  const NetMusicSourceInfo* source = active_source();
  const String list_name = source && source->list_name.length()
      ? source->list_name
      : String(kNetMusicListName);
  const size_t start = net_music_relative_path_start(list_name);
  const size_t encoded_len = encoded_net_music_path_length(list_name, start);
  (void)url.reserve(url.length() + encoded_len + 1);
  append_encoded_net_music_path(url, list_name, start);
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

  const uint32_t title_space_changes =
      text_normalize_display_spaces_inplace(item.title);
  const uint32_t artist_space_changes =
      text_normalize_display_spaces_inplace(item.artist);
  const uint32_t album_space_changes =
      text_normalize_display_spaces_inplace(item.album);
  if (title_space_changes + artist_space_changes + album_space_changes > 0u) {
    LOGD("[网络音乐] 已规范化标题空白：标题=%lu 歌手=%lu 专辑=%lu",
         (unsigned long)title_space_changes,
         (unsigned long)artist_space_changes,
         (unsigned long)album_space_changes);
  }

  item.title.trim();
  item.artist.trim();
  item.album.trim();

  if (!item.artist.length()) {
    item.artist = "NAS";
  }

  if (!item.album.length()) {
    item.album = "NAS";
  }

  item.valid =
      item.title.length() > 0 &&
      item.encoded_path.length() > 0 &&
      (item.format == "mp3" || item.format == "flac");

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
  if (!out || idx >= s_offsets_count) {
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

uint8_t net_music_catalog_source_count() {
  return s_source_count;
}

bool net_music_catalog_source_get(uint8_t idx, NetMusicSourceInfo* out) {
  if (!out || idx >= s_source_count || !s_sources[idx].valid) {
    return false;
  }

  *out = s_sources[idx];
  return true;
}

uint8_t net_music_catalog_active_source_index() {
  return s_active_source_idx;
}

String net_music_catalog_active_source_name() {
  const NetMusicSourceInfo* source = active_source();
  return source ? source->name : String("NAS音乐");
}

bool net_music_catalog_select_source(uint8_t idx) {
  if (idx >= s_source_count || !s_sources[idx].valid) {
    s_error = "net_music_source_out_of_range";
    return false;
  }

  if (idx == s_active_source_idx) {
    return true;
  }

  clear_loaded_list_state();
  s_active_source_idx = idx;
  s_error = "";

  if (!persist_active_source_index()) {
    LOGW("[网络音乐] 当前曲库源 NVS 保存失败：索引=%u", (unsigned)idx);
  }

  LOGI("[网络音乐] 已切换曲库源：索引=%u 名称=%s 路径=%s，仅保留当前列表",
       (unsigned)idx,
       s_sources[idx].name.c_str(),
       s_sources[idx].relative_path.c_str());
  return true;
}

bool net_music_catalog_load() {
  clear_loaded_list_state();
  s_error = "";

  // 打开 NAS 时只从 HTTP 下载列表到内存，不在 TF 卡读写 net_music.txt。
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
  uint32_t skipped_count = 0;
  uint32_t truncate_len = s_list_len;
  uint32_t pos = 0;

  while (pos < s_list_len) {
    const uint32_t line_start = pos;

    while (pos < s_list_len && s_list_buf[pos] != '\n') {
      ++pos;
    }

    bool kept_line = false;
    String line;
    if (copy_line_from_memory(line_start, line)) {
      NetMusicItem probe{};
      if (parse_line(line, &probe)) {
        if (valid_count < kNetMusicMaxItems) {
          if (!append_remote_offset(line_start)) {
            s_loaded = false;
            free_remote_offsets();
            free_remote_list_buffer();
            return false;
          }
          ++valid_count;
          kept_line = true;
        } else {
          ++skipped_count;
        }
      }
    }

    if (pos < s_list_len && s_list_buf[pos] == '\n') {
      ++pos;
    }

    if (kept_line) {
      truncate_len = pos;
    }
  }

  if (skipped_count > 0) {
    LOGW("[网络音乐] 有效条目超过上限: 保留=%lu 跳过=%lu",
         (unsigned long)valid_count,
         (unsigned long)skipped_count);
    if (truncate_len < s_list_len) {
      s_list_len = truncate_len;
      s_list_buf[s_list_len] = '\0';
      shrink_remote_list_capacity_to_len(s_list_len);
    }
  }

  if (valid_count == 0) {
    s_loaded = false;
    s_error = "net_music_no_valid_items";
    LOGW("[网络音乐] 远程列表没有有效 MP3 条目");
    free_remote_offsets();
    free_remote_list_buffer();
    return false;
  }

  s_loaded = true;
  s_error = "";

  const NetMusicSourceInfo* source = active_source();
  LOGI("[网络音乐] 列表加载完成：曲库=%s 歌曲=%lu 偏移=%u offset_bytes=%lu offset_psram=%d list_psram=%d base=%s 来源=memory",
       source ? source->name.c_str() : "NAS音乐",
       (unsigned long)valid_count,
       (unsigned)s_offsets_count,
       (unsigned long)((size_t)s_offsets_cap * sizeof(uint32_t)),
       s_offsets ? (esp_ptr_external_ram(s_offsets) ? 1 : 0) : 0,
       s_list_buf ? (esp_ptr_external_ram(s_list_buf) ? 1 : 0) : 0,
       build_active_source_root_url().c_str());

  return true;
}

bool net_music_catalog_is_loaded() {
  return s_loaded;
}

uint32_t net_music_catalog_count() {
  return s_offsets_count;
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

  for (uint32_t i = 0; i < s_offsets_count; ++i) {
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

  const size_t path_start =
      net_music_relative_path_start(item.encoded_path);
  if (path_start >= item.encoded_path.length()) {
    return String();
  }

  const size_t encoded_len =
      encoded_net_music_path_length(item.encoded_path, path_start);

  String url = build_active_source_root_url();
  if (!url.reserve(url.length() + encoded_len + 1)) {
    s_error = "net_music_url_alloc_failed";
    return String();
  }

  append_encoded_net_music_path(url, item.encoded_path, path_start);
  return url;
}

String net_music_catalog_base_url() {
  return build_active_source_root_url();
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

const char* net_music_catalog_sources_path() {
  return kNetMusicSourcesPath;
}

void net_music_catalog_clear() {
  clear_loaded_list_state();
  s_base_loaded = false;
  s_error = "";
  s_base_url = "";
  clear_source_config();
}