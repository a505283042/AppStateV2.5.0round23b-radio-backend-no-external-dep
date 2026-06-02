#pragma once
#include <Arduino.h>
#include <SdFat.h>

struct FlacBasicInfo {
  String title;
  String artist;
  String album;
};

// 只解析 Vorbis Comment（ARTIST/ALBUM/TITLE）；成功读取到 FLAC 头即返回 true
bool flac_read_vorbis_basic(SdFat& sd, const char* path, FlacBasicInfo& out);
