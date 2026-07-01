#pragma once
#include <Arduino.h>
#include <SdFat.h>

struct Mp3CoverLoc {
  bool found = false;
  uint32_t offset = 0;   // 封面二进制数据起始偏移（从文件0开始）
  uint32_t size = 0;     // 封面二进制数据长度
  String mime;           // image/jpeg / image/png
};

/**
 * @brief ID3/APIC 通用字节读取接口。
 *
 * 本地 TF 卡和 NAS HTTP Range 都可以实现这个接口，然后共用同一套
 * ID3v2 APIC 帧解析逻辑，避免重复维护两份解析器。
 */
class Id3ByteReader {
public:
  virtual ~Id3ByteReader() = default;
  virtual bool read(void* dst, size_t n) = 0;
  virtual int readByte() = 0;
  virtual bool seek(uint32_t pos) = 0;
  virtual bool skip(uint32_t n) = 0;
  virtual uint32_t position() const = 0;
};

bool id3_find_apic_from_reader(Id3ByteReader& reader, Mp3CoverLoc& out);
bool id3_find_apic(SdFat& sd, const char* path, Mp3CoverLoc& out);
