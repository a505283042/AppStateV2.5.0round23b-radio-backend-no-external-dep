#pragma once
#include <Arduino.h>
#include <SdFat.h>

struct FlacCoverLoc {
  bool found = false;
  uint64_t offset = 0;
  uint64_t size = 0;
  String mime;
};

/**
 * @brief FLAC 元数据通用字节读取接口。
 *
 * 本地 TF 卡和 NAS HTTP Range 都实现该接口，共用同一套 PICTURE block
 * 解析逻辑，避免远程封面与本地封面规则发生分叉。
 */
class FlacByteReader {
public:
  virtual ~FlacByteReader() = default;
  virtual bool read(void* dst, size_t n) = 0;
  virtual bool seek(uint32_t pos) = 0;
  virtual bool skip(uint32_t n) = 0;
  virtual uint32_t position() const = 0;
};

bool flac_find_picture_from_reader(FlacByteReader& reader, FlacCoverLoc& out);
bool flac_find_picture(SdFat& sd, const char* path, FlacCoverLoc& out);
