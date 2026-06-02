#pragma once
#include <Arduino.h>
#include <SdFat.h>

struct Mp3CoverLoc {
  bool found = false;
  uint32_t offset = 0;   // 封面二进制数据起始偏移（从文件0开始）
  uint32_t size = 0;     // 封面二进制数据长度
  String mime;           // image/jpeg / image/png
};

bool id3_find_apic(SdFat& sd, const char* path, Mp3CoverLoc& out);
