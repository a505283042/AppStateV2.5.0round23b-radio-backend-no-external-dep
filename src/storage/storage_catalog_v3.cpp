#include "storage/storage_catalog_v3.h"

#include "storage/storage_index_v3.h"
#include "storage/storage_builder_v3.h"
#include "storage/storage_view_v3.h"
#include "storage/storage_groups_v3.h"
#include "storage/storage_scan_v3.h"
#include "utils/log.h"
#include "app_diagnostics.h"

#include <esp_heap_caps.h>

static MusicCatalogV3 s_catalog_v3;
static bool s_v3_ready = false;
static uint32_t s_catalog_generation_seq = 0;

const MusicCatalogV3& storage_catalog_v3(void)
{
  return s_catalog_v3;
}

const std::vector<PlaylistGroup>& storage_catalog_v3_artist_groups(void)
{
  return s_catalog_v3.artist_groups;
}

const std::vector<PlaylistGroup>& storage_catalog_v3_album_groups(void)
{
  return s_catalog_v3.album_groups;
}

void storage_catalog_v3_clear(void)
{
  storage_catalog_v3_free(s_catalog_v3);
  s_v3_ready = false;
}

bool storage_catalog_v3_ready(void)
{
  return s_v3_ready && !s_catalog_v3.empty();
}

uint32_t storage_catalog_v3_track_count(void)
{
  return s_catalog_v3.track_count;
}

uint32_t storage_catalog_v3_album_count(void)
{
  return s_catalog_v3.album_count;
}

uint32_t storage_catalog_v3_artist_count(void)
{
  return s_catalog_v3.artist_count;
}

static bool try_load_v3(const char* v3_index_path)
{
  storage_catalog_v3_clear();

  if (!storage_index_load_v3(s_catalog_v3, v3_index_path)) {
    return false;
  }

  if (!storage_build_groups_v3(s_catalog_v3)) {
    LOGE("[曲库] 分组构建失败");
    storage_catalog_v3_clear();
    return false;
  }

  const char* load_source = storage_index_last_load_source_v3();
  if (storage_index_last_load_needs_rewrite_v3()) {
    // 旧版索引没有 CRC，或恢复文件未能提升为正式文件时，
    // 在完整加载和语义校验通过后立即用原子流程重写一次。
    if (storage_index_save_v3(s_catalog_v3, v3_index_path)) {
      LOGI("[曲库] 索引已升级为带 CRC 的原子格式：来源=%s", load_source);
    } else {
      LOGW("[曲库] 索引升级写回失败，当前内存曲库仍可继续使用：来源=%s", load_source);
    }
  }

  s_catalog_v3.generation = ++s_catalog_generation_seq;
  s_v3_ready = true;

  LOGI("[曲库] 加载成功：来源=%s 歌曲=%lu 专辑=%lu 歌手=%lu 字符池=%lu 歌手分组=%d 专辑分组=%d",
      load_source,
      (unsigned long)s_catalog_v3.track_count,
      (unsigned long)s_catalog_v3.album_count,
      (unsigned long)s_catalog_v3.artist_count,
      (unsigned long)s_catalog_v3.pool.size,
      (int)s_catalog_v3.artist_groups.size(),
      (int)s_catalog_v3.album_groups.size());

  // 强制打印曲库内存归因，不再依赖 boot_state 的 LOG_LEVEL 条件。
  // 这样开机加载、插卡重载、手动重建后都能确认 group vector 是否落到内部 RAM。
  storage_catalog_v3_log_memory_stats();

  return true;
}

static bool rebuild_v3_native(
    const char* music_root,
    const char* v3_index_path,
    StorageCatalogRebuildMode mode,
    StorageCatalogRebuildSummary* out_summary)
{
    static constexpr const char* kManifestPath =
        "/System/music_manifest_v1.bin";

    const uint32_t rebuild_started_ms = millis();
    const bool force_full_scan =
        mode == StorageCatalogRebuildMode::Full;

    StorageCatalogRebuildSummary summary{};
    summary.forced_full_scan = force_full_scan;

    StorageTrackBuildListV3 tmp_tracks;
    StorageMusicManifestV1 next_manifest;
    StorageIncrementalScanStatsV3 scan_stats{};

    LOGI("[曲库] 开始重建本地索引：请求模式=%s",
         force_full_scan ? "强制全量" : "增量优先");

    const MusicCatalogV3* reuse_catalog =
        (!force_full_scan && storage_catalog_v3_ready())
            ? &s_catalog_v3
            : nullptr;

    if (!storage_scan_music_incremental_v3(tmp_tracks,
                                           next_manifest,
                                           scan_stats,
                                           reuse_catalog,
                                           music_root,
                                           kManifestPath,
                                           force_full_scan)) {
        summary.full_scan = scan_stats.full_scan;
        summary.discovered = scan_stats.discovered;
        summary.reused = scan_stats.reused;
        summary.added = scan_stats.added;
        summary.modified = scan_stats.modified;
        summary.deleted = scan_stats.deleted;
        summary.elapsed_ms = millis() - rebuild_started_ms;
        if (out_summary) *out_summary = summary;
        LOGE("[曲库] scan 失败：模式=%s",
             scan_stats.full_scan ? "全量" : "增量");
        return false;
    }

    storage_catalog_v3_clear();

    if (!storage_build_catalog_v3_from_temp(tmp_tracks, s_catalog_v3)) {
        LOGE("[曲库] 从临时数据构建失败");
        storage_catalog_v3_clear();
        summary.full_scan = scan_stats.full_scan;
        summary.elapsed_ms = millis() - rebuild_started_ms;
        if (out_summary) *out_summary = summary;
        return false;
    }

    LOGI("[曲库][扫描内存] 临时曲目容器=%luB ext=%d，Catalog 构建后立即释放",
         (unsigned long)(tmp_tracks.capacity() * sizeof(TrackBuildTempV3)),
         (!tmp_tracks.empty() && esp_ptr_external_ram(tmp_tracks.data())) ? 1 : 0);
    StorageTrackBuildListV3 empty_tracks;
    tmp_tracks.swap(empty_tracks);

    if (!storage_build_groups_v3(s_catalog_v3)) {
        LOGE("[曲库] 分组构建失败");
        storage_catalog_v3_clear();
        summary.full_scan = scan_stats.full_scan;
        summary.elapsed_ms = millis() - rebuild_started_ms;
        if (out_summary) *out_summary = summary;
        return false;
    }

    s_catalog_v3.generation = ++s_catalog_generation_seq;
    s_v3_ready = true;

    // Manifest 与当前正式索引使用同一个 Catalog CRC 配对。
    // 如果索引已更新而 Manifest 保存失败，下次扫描会因 CRC 不匹配自动全量回退。
    next_manifest.catalog_crc32 =
        storage_manifest_catalog_crc_v1(s_catalog_v3);
    next_manifest.catalog_crc_valid = true;

    const bool index_saved =
        storage_index_save_v3(s_catalog_v3, v3_index_path);
    if (!index_saved) {
        LOGE("[曲库] 保存 v3 失败: %s", v3_index_path);
        LOGW("[曲库] 索引未落盘，本轮不更新增量清单");
    } else if (!storage_manifest_save_v1(next_manifest, kManifestPath)) {
        // 内存曲库和正式索引已经可用；清单失败只会使下一轮回退全量解析。
        LOGW("[曲库] 增量清单保存失败，下次重扫将自动回退全量解析");
    }

    scan_stats.elapsed_ms = millis() - rebuild_started_ms;

    LOGI("[曲库] 本地索引重建完成：扫描模式=%s 强制=%d 复用=%lu 新增=%lu 修改=%lu 删除=%lu 用时=%lums 歌曲=%lu 专辑=%lu 歌手=%lu 字符池=%lu 歌手分组=%d 专辑分组=%d",
         scan_stats.full_scan ? "全量" : "增量",
         force_full_scan ? 1 : 0,
         (unsigned long)scan_stats.reused,
         (unsigned long)scan_stats.added,
         (unsigned long)scan_stats.modified,
         (unsigned long)scan_stats.deleted,
         (unsigned long)scan_stats.elapsed_ms,
         (unsigned long)s_catalog_v3.track_count,
         (unsigned long)s_catalog_v3.album_count,
         (unsigned long)s_catalog_v3.artist_count,
         (unsigned long)s_catalog_v3.pool.size,
         (int)s_catalog_v3.artist_groups.size(),
         (int)s_catalog_v3.album_groups.size());

    storage_catalog_v3_log_memory_stats();

    summary.success = true;
    summary.full_scan = scan_stats.full_scan;
    summary.forced_full_scan = force_full_scan;
    summary.discovered = scan_stats.discovered;
    summary.reused = scan_stats.reused;
    summary.added = scan_stats.added;
    summary.modified = scan_stats.modified;
    summary.deleted = scan_stats.deleted;
    summary.elapsed_ms = scan_stats.elapsed_ms;
    if (out_summary) *out_summary = summary;

    return true;
}

bool storage_catalog_v3_load_or_rebuild(const char* music_root,
                                        const char* v3_index_path)
{
    if (try_load_v3(v3_index_path)) {
        return true;
    }

    LOGW("[曲库] 加载 v3 失败, 回退 native 重建");
    return rebuild_v3_native(music_root, v3_index_path,
                             StorageCatalogRebuildMode::Incremental, nullptr);
}

bool storage_catalog_v3_get_track_view(uint32_t track_index,
                                       TrackViewV3& out,
                                       const char* music_root)
{
  if (!storage_catalog_v3_ready()) {
    out = TrackViewV3{};
    return false;
  }

  return storage_make_track_view_v3(s_catalog_v3, track_index, out, music_root);
}

bool storage_catalog_v3_get_trackinfo(uint32_t track_index,
                                      TrackInfo& out,
                                      const char* music_root)
{
  if (!storage_catalog_v3_ready()) {
    out = TrackInfo{};
    return false;
  }

  return storage_fill_trackinfo_from_v3(s_catalog_v3, track_index, out, music_root);
}

#if APP_DIAG_RAM_ATTRIBUTION
static void log_ptr_region_v3(const char* label, const void* ptr, size_t bytes)
{
  LOGI("[内存归因] %s ptr=%p bytes=%lu internal=%d psram=%d",
       label,
       ptr,
       (unsigned long)bytes,
       ptr ? (esp_ptr_internal(ptr) ? 1 : 0) : 0,
       ptr ? (esp_ptr_external_ram(ptr) ? 1 : 0) : 0);
}

static void log_group_vector_regions_v3(const char* label, const std::vector<PlaylistGroup>& groups)
{
  size_t idx_bytes = 0;
  uint32_t internal_count = 0;
  uint32_t psram_count = 0;
  uint32_t unknown_count = 0;

  for (const auto& g : groups) {
    idx_bytes += g.track_indices.size() * sizeof(TrackIndex16);
    const void* p = g.track_indices.empty() ? nullptr : g.track_indices.data;
    if (!p) {
      continue;
    }
    if (esp_ptr_internal(p)) {
      ++internal_count;
    } else if (esp_ptr_external_ram(p)) {
      ++psram_count;
    } else {
      ++unknown_count;
    }
  }

  LOGI("[内存归因] %s groups=%u vector_array_ptr=%p vector_array_bytes=%lu internal=%d psram=%d idx_bytes=%lu idx_vectors_internal=%lu idx_vectors_psram=%lu idx_vectors_unknown=%lu",
       label,
       (unsigned)groups.size(),
       groups.empty() ? nullptr : groups.data(),
       (unsigned long)(groups.capacity() * sizeof(PlaylistGroup)),
       (!groups.empty() && esp_ptr_internal(groups.data())) ? 1 : 0,
       (!groups.empty() && esp_ptr_external_ram(groups.data())) ? 1 : 0,
       (unsigned long)idx_bytes,
       (unsigned long)internal_count,
       (unsigned long)psram_count,
       (unsigned long)unknown_count);
}

void storage_catalog_v3_log_memory_stats(void)
{
  if (!storage_catalog_v3_ready()) {
    LOGE("[曲库] 内存统计不可用: 目录 未就绪");
    return;
  }

  size_t track_bytes  = (size_t)s_catalog_v3.track_count  * sizeof(TrackRowV3);
  size_t album_bytes  = (size_t)s_catalog_v3.album_count  * sizeof(AlbumRowV3);
  size_t artist_bytes = (size_t)s_catalog_v3.artist_count * sizeof(ArtistRowV3);
  size_t pool_bytes   = (size_t)s_catalog_v3.pool.size;

  size_t groups_artist_idx_bytes = 0;
  for (const auto& g : s_catalog_v3.artist_groups) {
    groups_artist_idx_bytes += g.track_indices.size() * sizeof(TrackIndex16);
  }

  size_t groups_album_idx_bytes = 0;
  for (const auto& g : s_catalog_v3.album_groups) {
    groups_album_idx_bytes += g.track_indices.size() * sizeof(TrackIndex16);
  }

  size_t total_core = track_bytes + album_bytes + artist_bytes + pool_bytes;
  size_t total_with_groups = total_core + groups_artist_idx_bytes + groups_album_idx_bytes;

  log_ptr_region_v3("catalog.pool", s_catalog_v3.pool.data, pool_bytes);
  log_ptr_region_v3("catalog.tracks", s_catalog_v3.tracks, track_bytes);
  log_ptr_region_v3("catalog.albums", s_catalog_v3.albums, album_bytes);
  log_ptr_region_v3("catalog.artists", s_catalog_v3.artists, artist_bytes);
  log_ptr_region_v3("catalog.artist_group_track_pool",
                    s_catalog_v3.artist_group_track_pool,
                    (size_t)s_catalog_v3.artist_group_track_pool_count * sizeof(TrackIndex16));
  log_ptr_region_v3("catalog.album_group_track_pool",
                    s_catalog_v3.album_group_track_pool,
                    (size_t)s_catalog_v3.album_group_track_pool_count * sizeof(TrackIndex16));
  log_group_vector_regions_v3("catalog.artist_groups", s_catalog_v3.artist_groups);
  log_group_vector_regions_v3("catalog.album_groups", s_catalog_v3.album_groups);

  LOGD("[曲库][内存] 大小of(TrackRowV3)=%u 大小of(AlbumRowV3)=%u 大小of(ArtistRowV3)=%u",
       (unsigned)sizeof(TrackRowV3),
       (unsigned)sizeof(AlbumRowV3),
       (unsigned)sizeof(ArtistRowV3));

  LOGD("[曲库][内存] 歌曲s=%lu -> %lu 字节",
       (unsigned long)s_catalog_v3.track_count,
       (unsigned long)track_bytes);

  LOGD("[曲库][内存] 专辑s=%lu -> %lu 字节",
       (unsigned long)s_catalog_v3.album_count,
       (unsigned long)album_bytes);

  LOGD("[曲库][内存] 歌手s=%lu -> %lu 字节",
       (unsigned long)s_catalog_v3.artist_count,
       (unsigned long)artist_bytes);

  LOGD("[曲库][内存] string_pool=%lu 字节",
       (unsigned long)pool_bytes);

  LOGD("[曲库][内存] 歌手_分组s=%d idx_字节=%lu",
       (int)s_catalog_v3.artist_groups.size(),
       (unsigned long)groups_artist_idx_bytes);

  LOGD("[曲库][内存] 专辑_分组s=%d idx_字节=%lu",
       (int)s_catalog_v3.album_groups.size(),
       (unsigned long)groups_album_idx_bytes);

  LOGD("[曲库][内存] core_总计=%lu 字节 (%.2f KB, %.2f MB)",
       (unsigned long)total_core,
       total_core / 1024.0f,
       total_core / 1024.0f / 1024.0f);

  LOGD("[曲库][内存] 总计_with_分组s=%lu 字节 (%.2f KB, %.2f MB)",
       (unsigned long)total_with_groups,
       total_with_groups / 1024.0f,
       total_with_groups / 1024.0f / 1024.0f);

  LOGD("[曲库][内存] note: 分组 对象/String 额外开销未完全计入");
}

#else
void storage_catalog_v3_log_memory_stats(void)
{
}
#endif

bool storage_catalog_v3_rebuild(
    const char* music_root,
    const char* v3_index_path,
    StorageCatalogRebuildMode mode,
    StorageCatalogRebuildSummary* out_summary)
{
    return rebuild_v3_native(
        music_root, v3_index_path, mode, out_summary);
}
