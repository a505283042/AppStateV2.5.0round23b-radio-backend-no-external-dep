#pragma once
#include <Arduino.h>
#include <vector>
#include "storage/storage_types_v3.h"

/**
 * @brief V3 扫描阶段的临时曲目结构。
 *
 * 只在扫描 / builder 阶段存在；后续会被压缩为 TrackRowV3 + StringPool。
 */
struct TrackBuildTempV3 {
  String title;
  String artist;
  String album;

  String audio_rel;       // 相对 /Music 的路径
  String lrc_rel;         // 相对 /Music 的路径，无则空
  String cover_path_rel;  // fallback 相对路径，无则空
  String cover_mime;

  uint32_t cover_offset = 0;
  uint32_t cover_size = 0;

  uint8_t cover_source = COVER_NONE;
  uint8_t ext_code = EXT_UNKNOWN;
  uint16_t flags = TF_NONE;
};

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
bool storage_scan_music_v3(std::vector<TrackBuildTempV3>& out_tracks,
                           const char* music_root = "/Music");
