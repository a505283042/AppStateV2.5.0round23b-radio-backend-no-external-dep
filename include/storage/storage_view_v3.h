#pragma once
#include <Arduino.h>
#include "storage/storage_types_v3.h"

/* 只读展开视图：给 UI / player / 调试打印用 */
struct TrackViewV3 {
  bool valid = false;
  uint32_t track_index = 0;

  String title;
  String artist;
  String album;

  String audio_path;   // 完整路径
  String lrc_path;     // 完整路径，无则空
  String cover_path;   // 完整路径，无则空
  String cover_mime;

  uint32_t cover_offset = 0;
  uint32_t cover_size = 0;

  uint8_t cover_source = COVER_NONE;
  uint8_t ext_code = EXT_UNKNOWN;
  uint16_t flags = TF_NONE;
  uint32_t album_id = INVALID_ID32;
};

/* 从 V3 catalog 取一首歌的展开视图 */
bool storage_make_track_view_v3(const MusicCatalogV3& cat,
                                uint32_t track_index,
                                TrackViewV3& out,
                                const char* music_root = "/Music");

/* 从 V3 catalog 直接生成兼容层 TrackInfo，供现有代码复用 */
bool storage_fill_legacy_trackinfo_from_v3(const MusicCatalogV3& cat,
                                           uint32_t track_index,
                                           TrackInfo& out,
                                           const char* music_root = "/Music");