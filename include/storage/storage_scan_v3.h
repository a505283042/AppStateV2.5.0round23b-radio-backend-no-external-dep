#pragma once
#include <Arduino.h>
#include "storage/storage_types_v3.h"
#include "utils/psram_containers.h"
#include "storage/storage_manifest_v1.h"

/**
 * @brief V3 扫描阶段的临时曲目结构。
 *
 * 只在扫描 / builder 阶段存在；后续会被压缩为 TrackRowV3 + StringPool。
 */

/**
 * @brief 增量扫描统计。
 */
struct StorageIncrementalScanStatsV3 {
  bool manifest_loaded = false;
  bool full_scan = true;
  bool forced_full_scan = false;
  bool strict_incremental = false;
  uint32_t discovered = 0;
  uint32_t reused = 0;
  uint32_t added = 0;
  uint32_t modified = 0;
  uint32_t deleted = 0;
  uint32_t attribute_reused = 0;
  uint32_t content_verified = 0;
  uint32_t elapsed_ms = 0;
};

struct TrackBuildTempV3 {
  PsramString title;
  PsramString artist;
  PsramString album;

  PsramString audio_rel;       // 相对 /Music 的路径
  PsramString lrc_rel;         // 相对 /Music 的路径，无则空
  PsramString cover_path_rel;  // fallback 相对路径，无则空
  PsramString cover_mime;

  uint32_t cover_offset = 0;
  uint32_t cover_size = 0;

  uint8_t cover_source = COVER_NONE;
  uint8_t ext_code = EXT_UNKNOWN;
  uint16_t flags = TF_NONE;
};

using StorageTrackBuildListV3 = PsramVector<TrackBuildTempV3>;
using StorageTrackIndexListV3 = PsramVector<uint32_t>;
using StorageTrackSeenListV3 = PsramVector<uint8_t>;

/**
 * @brief 扫描单个音频文件并提取元数据 / 歌词 / 封面信息。
 * @param fallback_* 来自目录层级或文件系统推导的兜底信息。
 */
bool storage_scan_one_audio_file_v3(const String& full_path,
                                    const String& fallback_artist,
                                    const String& fallback_album,
                                    const String& fallback_cover_path,
                                    TrackBuildTempV3& out_track);

/**
 * @brief 递归扫描 /Music，输出 V3 builder 使用的临时曲目列表。
 *
 * 当前实现包含：
 * - 目录递归遍历
 * - 封面优先候选名搜索
 * - 周期性让出 CPU，避免 rescan_v3 扫描时触发 WDT
 */
bool storage_scan_music_v3(StorageTrackBuildListV3& out_tracks,
                           const char* music_root = "/Music");

/**
 * @brief 使用 Manifest 执行增量扫描。
 *
 * 首次没有有效 Manifest 时自动执行全量解析，并生成下一份 Manifest。
 * 后续扫描仍遍历目录，但未变化歌曲只读取轻量指纹并复用旧元数据。
 */
bool storage_scan_music_incremental_v3(
    StorageTrackBuildListV3& out_tracks,
    StorageMusicManifestV1& out_manifest,
    StorageIncrementalScanStatsV3& out_stats,
    const MusicCatalogV3* reuse_catalog,
    const char* music_root = "/Music",
    const char* manifest_path = "/System/music_manifest_v1.bin",
    bool force_full_scan = false,
    bool strict_verify = false);
