#pragma once

#include <Arduino.h>
#include "storage/storage_types_v3.h"
#include "utils/psram_containers.h"

/**
 * @brief 单个文件的轻量内容指纹。
 *
 * 不依赖 FAT 修改时间，使用文件大小以及头部、中部、尾部采样 CRC。
 * 这样复制工具保留时间戳、或文件内容等长替换时仍有较高概率检测到变化。
 */
struct StorageFileFingerprintV1 {
  bool present = false;
  uint32_t size = 0;
  uint32_t head_crc = 0;
  uint32_t middle_crc = 0;
  uint32_t tail_crc = 0;
};

/**
 * @brief 增量曲库清单中的单曲记录。
 *
 * 只保存音频路径和三个文件指纹。歌词路径、封面路径、标题、歌手和专辑等
 * 都从当前已加载的 V3 Catalog 复用，尽量减少扫描期间的 String 数量和内部堆压力。
 */
struct StorageManifestEntryV1 {
  PsramString audio_rel;

  StorageFileFingerprintV1 audio_fingerprint;
  StorageFileFingerprintV1 lrc_fingerprint;
  StorageFileFingerprintV1 cover_fingerprint;
};

struct StorageMusicManifestV1 {
  PsramVector<StorageManifestEntryV1> entries;

  void clear() {
    entries.clear();
  }

  bool empty() const {
    return entries.empty();
  }
};

/**
 * @brief 从原子清单文件加载。正式文件不可用时会尝试 .bak。
 */
bool storage_manifest_load_v1(
    StorageMusicManifestV1& out_manifest,
    const char* manifest_path = "/System/music_manifest_v1.bin");

/**
 * @brief 使用 .tmp + .bak 原子保存清单，并在替换前后执行 CRC 校验。
 */
bool storage_manifest_save_v1(
    const StorageMusicManifestV1& manifest,
    const char* manifest_path = "/System/music_manifest_v1.bin");
