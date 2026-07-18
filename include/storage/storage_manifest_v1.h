#pragma once

#include <Arduino.h>
#include "storage/storage_types_v3.h"
#include "utils/psram_containers.h"

/**
 * @brief 单个文件的轻量内容指纹。
 *
 * 快速增量优先比较 FAT 修改日期、时间和文件大小；严格增量再比较
 * 文件头部、中部、尾部采样 CRC。旧版清单没有 FAT 属性时会自动做一次 CRC 升级。
 */
struct StorageFileFingerprintV1 {
  bool present = false;
  bool attributes_valid = false;
  uint32_t size = 0;
  uint16_t modify_date = 0;
  uint16_t modify_time = 0;
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

struct StorageDirectorySnapshotV1 {
  // 相对 /Music 的目录路径；空字符串代表 /Music 根目录。
  PsramString dir_rel;
  StorageFileFingerprintV1 directory_attributes;
  PsramString effective_cover_rel;
  StorageFileFingerprintV1 effective_cover_attributes;
  bool has_local_cover = false;
  bool has_subdirectories = false;
  uint32_t subtree_track_count = 0;
};

struct StorageMusicManifestV1 {
  PsramVector<StorageManifestEntryV1> entries;
  PsramVector<StorageDirectorySnapshotV1> directories;
  uint32_t catalog_crc32 = 0;
  bool catalog_crc_valid = false;
  uint16_t format_version = 0;

  void clear() {
    entries.clear();
    directories.clear();
    catalog_crc32 = 0;
    catalog_crc_valid = false;
    format_version = 0;
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

/** 计算当前 Catalog 的稳定一致性标识，供 Manifest 与索引配对校验。 */
uint32_t storage_manifest_catalog_crc_v1(const MusicCatalogV3& catalog);
