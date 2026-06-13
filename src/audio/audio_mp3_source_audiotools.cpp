// MP3 HTTP stream source adapter
// 职责：
// 1) 使用 WiFiClient 直接打开 HTTP MP3/ICY 网络流
// 2) 解析并跳过 HTTP 响应头，支持 301/302/307/308 一次跳转
// 3) 适配为 AudioMp3Source
// 4) 不负责 MP3 解码

#include "audio/audio_mp3_source_audiotools.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>

#include "utils/log.h"

#define LOG_TAG "APP"

namespace {

static WiFiClient g_client;
static String s_url;
static bool s_open = false;

constexpr uint16_t kDefaultHttpPort = 80;
constexpr uint32_t kConnectTimeoutMs = 5000;
constexpr uint32_t kHeaderTimeoutMs = 6000;
constexpr int kMaxRedirects = 2;

struct ParsedUrl {
  String host;
  String path;
  uint16_t port = kDefaultHttpPort;
};

static bool parse_http_url(const char* url, ParsedUrl& out)
{
  if (!url || !*url) return false;

  String u(url);
  u.trim();

  const char* prefix = "http://";
  if (!u.startsWith(prefix)) {
    LOGE("[RADIO] only http stream is supported: %s", url);
    return false;
  }

  String rest = u.substring(strlen(prefix));
  const int slash = rest.indexOf('/');
  String host_port = slash >= 0 ? rest.substring(0, slash) : rest;
  out.path = slash >= 0 ? rest.substring(slash) : String("/");

  if (host_port.length() == 0) return false;

  const int colon = host_port.lastIndexOf(':');
  if (colon > 0) {
    out.host = host_port.substring(0, colon);
    const int port = host_port.substring(colon + 1).toInt();
    out.port = port > 0 ? static_cast<uint16_t>(port) : kDefaultHttpPort;
  } else {
    out.host = host_port;
    out.port = kDefaultHttpPort;
  }

  return out.host.length() > 0 && out.path.length() > 0;
}

static bool read_line_with_timeout(String& line, uint32_t timeout_ms)
{
  line = "";
  const uint32_t start = millis();

  while (millis() - start < timeout_ms) {
    while (g_client.available() > 0) {
      const char c = static_cast<char>(g_client.read());
      if (c == '\r') continue;
      if (c == '\n') return true;
      line += c;
      if (line.length() > 512) {
        LOGW("[RADIO] HTTP header line too long");
        return false;
      }
    }

    if (!g_client.connected()) {
      return line.length() > 0;
    }

    delay(5);
  }

  LOGW("[RADIO] HTTP header read timeout");
  return false;
}

static int parse_status_code(const String& status_line)
{
  // 支持 HTTP/1.x 200，也兼容部分 ICY 200 OK 响应。
  if (status_line.startsWith("ICY ")) {
    return status_line.substring(4, 7).toInt();
  }

  const int first_space = status_line.indexOf(' ');
  if (first_space < 0 || first_space + 3 > status_line.length()) return 0;
  return status_line.substring(first_space + 1, first_space + 4).toInt();
}

static String make_redirect_url(const ParsedUrl& current, const String& location)
{
  String loc = location;
  loc.trim();

  if (loc.startsWith("http://") || loc.startsWith("https://")) {
    return loc;
  }

  if (loc.startsWith("/")) {
    String base = "http://" + current.host;
    if (current.port != kDefaultHttpPort) {
      base += ":";
      base += String(current.port);
    }
    return base + loc;
  }

  // 相对路径：按当前目录拼接。
  String dir = current.path;
  const int last_slash = dir.lastIndexOf('/');
  dir = last_slash >= 0 ? dir.substring(0, last_slash + 1) : String("/");

  String base = "http://" + current.host;
  if (current.port != kDefaultHttpPort) {
    base += ":";
    base += String(current.port);
  }
  return base + dir + loc;
}

static bool read_response_header(const ParsedUrl& current, int& status_code, String& location)
{
  String line;
  if (!read_line_with_timeout(line, kHeaderTimeoutMs)) {
    LOGE("[RADIO] HTTP status line missing");
    return false;
  }

  status_code = parse_status_code(line);
  LOGD("[RADIO] HTTP status: %s", line.c_str());

  while (read_line_with_timeout(line, kHeaderTimeoutMs)) {
    if (line.length() == 0) {
      return true;
    }

    const int colon = line.indexOf(':');
    if (colon > 0) {
      String key = line.substring(0, colon);
      String value = line.substring(colon + 1);
      key.trim();
      value.trim();
      key.toLowerCase();

      if (key == "location") {
        location = make_redirect_url(current, value);
      } else if (key == "content-type") {
        LOGD("[RADIO] HTTP content-type: %s", value.c_str());
      } else if (key == "icy-name") {
        LOGD("[RADIO] ICY name: %s", value.c_str());
      }
    }
  }

  LOGE("[RADIO] HTTP header not completed");
  return false;
}

static bool open_http_stream_once(const char* url, String& redirect_url)
{
  redirect_url = String();

  ParsedUrl parsed{};
  if (!parse_http_url(url, parsed)) {
    LOGE("[RADIO] invalid stream url: %s", url ? url : "<null>");
    return false;
  }

  LOGD("[RADIO] HTTP connect host=%s port=%u path=%s", parsed.host.c_str(), parsed.port, parsed.path.c_str());

  g_client.stop();
  g_client.setTimeout(kConnectTimeoutMs);

  if (!g_client.connect(parsed.host.c_str(), parsed.port)) {
    LOGE("[RADIO] HTTP connect failed host=%s port=%u", parsed.host.c_str(), parsed.port);
    return false;
  }

  // 使用 HTTP/1.0，避免部分电台 chunked/keep-alive 行为导致流读取不稳定。
  g_client.print("GET ");
  g_client.print(parsed.path);
  g_client.print(" HTTP/1.0\r\n");
  g_client.print("Host: ");
  g_client.print(parsed.host);
  if (parsed.port != kDefaultHttpPort) {
    g_client.print(":");
    g_client.print(parsed.port);
  }
  g_client.print("\r\n");
  g_client.print("User-Agent: ESP32S3-Player/1.0\r\n");
  g_client.print("Accept: audio/mpeg, audio/mp3, */*\r\n");
  g_client.print("Icy-MetaData: 0\r\n");
  g_client.print("Connection: close\r\n\r\n");

  int status_code = 0;
  String location;
  if (!read_response_header(parsed, status_code, location)) {
    g_client.stop();
    return false;
  }

  if (status_code == 301 || status_code == 302 || status_code == 307 || status_code == 308) {
    if (location.length() == 0) {
      LOGE("[RADIO] HTTP redirect without Location");
      g_client.stop();
      return false;
    }

    LOGD("[RADIO] HTTP redirect -> %s", location.c_str());
    redirect_url = location;
    g_client.stop();
    return false;
  }

  if (status_code < 200 || status_code >= 300) {
    LOGE("[RADIO] HTTP bad status=%d url=%s", status_code, url);
    g_client.stop();
    return false;
  }

  return true;
}

static int http_source_read(void* ctx, uint8_t* dst, size_t bytes)
{
  auto* client = static_cast<WiFiClient*>(ctx);
  if (!client || !dst || bytes == 0) return AUDIO_MP3_SOURCE_ERROR;
  if (!s_open) return AUDIO_MP3_SOURCE_EOF;

  // WiFi 已关闭时直接结束，避免进入底层 socket read。
  // 注意：不要在 available() 之前用 client->connected() 判死，
  // 某些 HTTP/1.0 流在 connected=false 时仍可能有缓冲数据可读。
  if (!WiFi.isConnected()) {
    s_open = false;
    return AUDIO_MP3_SOURCE_EOF;
  }

  int avail = client->available();
  if (avail <= 0) {
    if (!client->connected()) {
      s_open = false;
      return AUDIO_MP3_SOURCE_EOF;
    }
    return AUDIO_MP3_SOURCE_WOULD_BLOCK;
  }

  size_t want = bytes;
  if ((size_t)avail < want) want = (size_t)avail;

  const int n = client->read(dst, want);
  if (n > 0) return n;

  if (!WiFi.isConnected() || !client->connected()) {
    s_open = false;
    return AUDIO_MP3_SOURCE_EOF;
  }

  return AUDIO_MP3_SOURCE_WOULD_BLOCK;
}

static void http_source_close_impl(void* ctx)
{
  auto* client = static_cast<WiFiClient*>(ctx);
  if (client) {
    client->stop();
  }
  s_open = false;
  s_url = String();
}

} // namespace

bool audio_mp3_audiotools_source_open(const char* url, AudioMp3Source& out_source)
{
  audio_mp3_audiotools_source_close();

  if (!url || !*url) {
    LOGE("[RADIO] HTTP source open failed: empty url");
    return false;
  }

  if (!WiFi.isConnected()) {
    LOGE("[RADIO] HTTP source open failed: WiFi not connected");
    return false;
  }

  String current_url(url);
  bool ok = false;

  for (int attempt = 0; attempt <= kMaxRedirects; ++attempt) {
    String redirect_url;
    ok = open_http_stream_once(current_url.c_str(), redirect_url);
    if (ok) break;

    if (redirect_url.length() == 0) {
      break;
    }

    current_url = redirect_url;
  }

  if (!ok) {
    LOGE("[RADIO] HTTP stream open failed: %s", url);
    return false;
  }

  s_open = true;
  s_url = current_url;

  out_source = AudioMp3Source{};
  out_source.ctx = &g_client;
  out_source.read = http_source_read;
  out_source.close = http_source_close_impl;
  out_source.debug_name = s_url.c_str();
  out_source.is_stream = true;

  LOGD("[RADIO] HTTP stream open ok: %s", s_url.c_str());
  return true;
}

void audio_mp3_audiotools_source_close()
{
  if (s_open) {
    g_client.stop();
    s_open = false;
  }
  s_url = String();
}

bool audio_mp3_audiotools_source_is_open()
{
  return s_open;
}

const char* audio_mp3_audiotools_source_url()
{
  return s_url.c_str();
}

int audio_mp3_audiotools_source_available()
{
  if (!s_open) return 0;
  int avail = g_client.available();
  return avail > 0 ? avail : 0;
}

bool audio_mp3_audiotools_source_connected()
{
  return s_open && WiFi.isConnected() && g_client.connected();
}