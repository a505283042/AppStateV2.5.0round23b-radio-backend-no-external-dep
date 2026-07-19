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
static uint32_t s_audio_data_offset = 0;

static uint32_t detect_audio_data_offset()
{
  uint8_t header[10] = {0};
  if (g_file.seek(0) && g_file.read(header, sizeof(header)) == (ssize_t)sizeof(header) &&
      header[0] == 'I' && header[1] == 'D' && header[2] == '3') {
    const uint32_t tag_size =
        ((uint32_t)(header[6] & 0x7F) << 21) |
        ((uint32_t)(header[7] & 0x7F) << 14) |
        ((uint32_t)(header[8] & 0x7F) << 7) |
        ((uint32_t)(header[9] & 0x7F));
    uint32_t offset = 10u + tag_size;
    if (header[3] >= 4 && (header[5] & 0x10)) offset += 10u;
    if (offset < g_file.size()) {
      (void)g_file.seek(0);
      return offset;
    }
  }
  (void)g_file.seek(0);
  return 0;
}

static int file_source_read(void* ctx, uint8_t* dst, size_t bytes)
{
  (void)ctx;
  if (!dst || bytes == 0) return AUDIO_MP3_SOURCE_ERROR;
  if (!s_open) return AUDIO_MP3_SOURCE_EOF;

  const ssize_t n = g_file.read(dst, bytes);
  if (n > 0) return (int)n;
  if (n == 0) return AUDIO_MP3_SOURCE_EOF;
  return AUDIO_MP3_SOURCE_ERROR;
}

static bool file_source_seek(void* ctx, uint32_t absolute_offset)
{
  (void)ctx;
  return s_open && g_file.seek(absolute_offset);
}

static uint32_t file_source_tell(void* ctx)
{
  (void)ctx;
  return s_open ? g_file.tell() : 0;
}

static uint32_t file_source_size(void* ctx)
{
  (void)ctx;
  return s_open ? g_file.size() : 0;
}

static void file_source_close(void* ctx)
{
  (void)ctx;
  if (s_open) {
    g_file.close();
    s_open = false;
  }
  s_audio_data_offset = 0;
  s_path = String();
}

} // namespace

bool audio_mp3_file_source_open(SdFat& sd, const char* path, AudioMp3Source& out_source)
{
  audio_mp3_file_source_close();

  if (!path || !*path) {
    LOGE("[MP3] 文件音源打开失败：路径为空");
    return false;
  }

  if (!g_file.open(sd, path)) {
    LOGE("[MP3] 打开失败：%s", path);
    return false;
  }

  s_open = true;
  s_path = String(path);
  s_audio_data_offset = detect_audio_data_offset();

  out_source = AudioMp3Source{};
  out_source.ctx = nullptr;
  out_source.read = file_source_read;
  out_source.seek = file_source_seek;
  out_source.tell = file_source_tell;
  out_source.size = file_source_size;
  out_source.close = file_source_close;
  out_source.debug_name = s_path.c_str();
  out_source.is_stream = false;
  out_source.audio_data_offset = s_audio_data_offset;
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