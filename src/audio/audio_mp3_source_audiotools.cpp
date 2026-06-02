// MP3 AudioTools source adapter
// 职责：
// 1) 从 AudioTools URLStream 读取 MP3 字节
// 2) 适配为 AudioMp3Source
// 3) 提供 available/connected/url 等轻量状态
// 4) 不负责 MP3 解码

#include "audio/audio_mp3_source_audiotools.h"

#include <Arduino.h>
#include <WiFi.h>

#include "AudioTools.h"
#include "AudioTools/Communication/AudioHttp.h"

#ifdef LOGD
  #undef LOGD
#endif
#ifdef LOGI
  #undef LOGI
#endif
#ifdef LOGW
  #undef LOGW
#endif
#ifdef LOGE
  #undef LOGE
#endif

#include "utils/log.h"

#define LOG_TAG "APP"

using namespace audio_tools;

namespace {

static URLStream g_url_stream;
static String s_url;
static bool s_open = false;

static int audiotools_source_read(void* ctx, uint8_t* dst, size_t bytes)
{
  auto* stream = static_cast<URLStream*>(ctx);
  if (!stream || !dst || bytes == 0) return AUDIO_MP3_SOURCE_ERROR;
  if (!s_open) return AUDIO_MP3_SOURCE_EOF;

  int avail = stream->available();
  if (avail <= 0) {
    if (!WiFi.isConnected()) return AUDIO_MP3_SOURCE_EOF;
    return AUDIO_MP3_SOURCE_WOULD_BLOCK;
  }

  size_t want = bytes;
  if ((size_t)avail < want) want = (size_t)avail;

  size_t n = stream->readBytes(dst, want);
  if (n > 0) return (int)n;

  return AUDIO_MP3_SOURCE_WOULD_BLOCK;
}

static void audiotools_source_close_impl(void* ctx)
{
  auto* stream = static_cast<URLStream*>(ctx);
  if (stream) {
    stream->end();
  }
  s_open = false;
  s_url = String();
}

} // namespace

bool audio_mp3_audiotools_source_open(const char* url, AudioMp3Source& out_source)
{
  audio_mp3_audiotools_source_close();

  if (!url || !*url) {
    LOGE("[RADIO] AudioTools source open failed: empty url");
    return false;
  }

  bool ok = g_url_stream.begin(url, "audio/mp3");
  if (!ok) {
    LOGE("[RADIO] AudioTools begin failed: %s", url);
    return false;
  }

  s_open = true;
  s_url = String(url);

  out_source = AudioMp3Source{};
  out_source.ctx = &g_url_stream;
  out_source.read = audiotools_source_read;
  out_source.close = audiotools_source_close_impl;
  out_source.debug_name = s_url.c_str();
  out_source.is_stream = true;
  return true;
}

void audio_mp3_audiotools_source_close()
{
  if (s_open) {
    g_url_stream.end();
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
  int avail = g_url_stream.available();
  return avail > 0 ? avail : 0;
}

bool audio_mp3_audiotools_source_connected()
{
  return s_open && WiFi.isConnected();
}