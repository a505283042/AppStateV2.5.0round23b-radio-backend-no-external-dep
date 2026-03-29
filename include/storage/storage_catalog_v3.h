#pragma once
#include <Arduino.h>
#include <vector>
#include <cstdint>
#include "storage/storage_types_v3.h"
#include "storage/storage_view_v3.h"
#include "storage/storage_groups_v3.h"

/**
 * @brief 全局 V3 catalog 访问入口。
 *
 * 负责统一管理：
 * - 载入 V3 索引
 * - 失败时强制 native rebuild
 * - 只读访问 tracks / albums / artists / groups
 */

/* 访问全局 V3 catalog */
const MusicCatalogV3& storage_catalog_v3(void);

/* 只读访问 V3 groups */
const std::vector<PlaylistGroup>& storage_catalog_v3_artist_groups(void);
const std::vector<PlaylistGroup>& storage_catalog_v3_album_groups(void);

/* 清空全局 V3 catalog */
void storage_catalog_v3_clear(void);

/* 释放一个临时 catalog 实例的内存 */
void storage_catalog_v3_free(MusicCatalogV3& cat);

/*
 * 统一入口：
 * 1) 优先加载 V3 索引
 * 2) 失败则走 native rebuild
 * 3) 成功后保存最新 V3
 */
bool storage_catalog_v3_load_or_rebuild(const char* music_root = "/Music",
                                        const char* v3_index_path = "/System/music_index_v3.bin");

/* 强制重扫并重建 V3（跳过索引加载） */
bool storage_catalog_v3_rebuild(const char* music_root = "/Music",
                                const char* v3_index_path = "/System/music_index_v3.bin");

/* 基本访问 */
bool storage_catalog_v3_ready(void);
uint32_t storage_catalog_v3_track_count(void);
uint32_t storage_catalog_v3_album_count(void);
uint32_t storage_catalog_v3_artist_count(void);

/* 取只读 TrackViewV3 */
bool storage_catalog_v3_get_track_view(uint32_t track_index,
                                       TrackViewV3& out,
                                       const char* music_root = "/Music");

/* 取兼容层 TrackInfo，供旧播放链路复用 */
bool storage_catalog_v3_get_legacy_trackinfo(uint32_t track_index,
                                             TrackInfo& out,
                                             const char* music_root = "/Music");

/* 输出内存统计日志，便于看 builder / catalog 占用。 */
void storage_catalog_v3_log_memory_stats(void);
