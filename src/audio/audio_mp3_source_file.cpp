// MP3 file source adapter
// 职责：
// 1) 从 SD 文件读取 MP3 字节
// 2) 适配为 AudioMp3Source
// 3) 不负责 MP3 解码

#include "audio/audio_mp3_source_file.h"

#include <Arduino.h>

#include "audio/audio_file.h"
#include "utils/log.h"

#define LOG_TAG "APP"

namespace {

static AudioFile g_file;
static String s_path;
static bool s_open = false;

static int file_source_read(void* ctx, uint8_t* dst, size_t bytes)
{
  (void)ctx;
  if (!dst || bytes == 0) return AUDIO_MP3_SOURCE_ERROR;
  if (!s_open) return AUDIO_MP3_SOURCE_EOF;

  int n = g_file.read(dst, bytes);
  if (n > 0) return n;
  return AUDIO_MP3_SOURCE_EOF;
}

static void file_source_close(void* ctx)
{
  (void)ctx;
  if (s_open) {
    g_file.close();
    s_open = false;
  }
  s_path = String();
}

} // namespace

bool audio_mp3_file_source_open(SdFat& sd, const char* path, AudioMp3Source& out_source)
{
  audio_mp3_file_source_close();

  if (!path || !*path) {
    LOGE("[MP3] file source open failed: empty path");
    return false;
  }

  if (!g_file.open(sd, path)) {
    LOGE("[MP3] open failed: %s", path);
    return false;
  }

  s_open = true;
  s_path = String(path);

  out_source = AudioMp3Source{};
  out_source.ctx = nullptr;
  out_source.read = file_source_read;
  out_source.close = file_source_close;
  out_source.debug_name = s_path.c_str();
  out_source.is_stream = false;
  return true;
}

void audio_mp3_file_source_close()
{
  if (s_open) {
    g_file.close();
    s_open = false;
  }
  s_path = String();
}

bool audio_mp3_file_source_is_open()
{
  return s_open;
}

const char* audio_mp3_file_source_path()
{
  return s_path.c_str();
}