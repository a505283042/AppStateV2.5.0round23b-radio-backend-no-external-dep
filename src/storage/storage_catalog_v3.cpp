#include "storage/storage_catalog_v3.h"

#include "storage/storage_index_v3.h"
#include "storage/storage_builder_v3.h"
#include "storage/storage_view_v3.h"
#include "storage/storage_groups_v3.h"
#include "storage/storage_scan_v3.h"
#include "utils/log.h"

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

  storage_build_groups_v3(s_catalog_v3);

  s_catalog_v3.generation = ++s_catalog_generation_seq;
  s_v3_ready = true;

  LOGI("[曲库] 加载成功：歌曲=%lu 专辑=%lu 歌手=%lu 字符池=%lu 歌手分组=%d 专辑分组=%d",
       (unsigned long)s_catalog_v3.track_count,
       (unsigned long)s_catalog_v3.album_count,
       (unsigned long)s_catalog_v3.artist_count,
       (unsigned long)s_catalog_v3.pool.size,
       (int)s_catalog_v3.artist_groups.size(),
       (int)s_catalog_v3.album_groups.size());

  return true;
}

static bool rebuild_v3_native(const char* music_root,
                              const char* v3_index_path)
{
    std::vector<TrackBuildTempV3> tmp_tracks;

    LOGI("[曲库] 开始重建本地索引...");

    if (!storage_scan_music_v3(tmp_tracks, music_root)) {
        LOGE("[曲库] native scan 失败");
        return false;
    }

    storage_catalog_v3_clear();

    if (!storage_build_catalog_v3_from_temp(tmp_tracks, s_catalog_v3)) {
        LOGE("[曲库] 从临时数据构建失败");
        storage_catalog_v3_clear();
        return false;
    }

    storage_build_groups_v3(s_catalog_v3);

    s_catalog_v3.generation = ++s_catalog_generation_seq;
    s_v3_ready = true;

    if (!storage_index_save_v3(s_catalog_v3, v3_index_path)) {
        LOGE("[曲库] 保存 v3 失败: %s", v3_index_path);
    }

    LOGI("[曲库] 本地索引重建完成：歌曲=%lu 专辑=%lu 歌手=%lu 字符池=%lu 歌手分组=%d 专辑分组=%d",
         (unsigned long)s_catalog_v3.track_count,
         (unsigned long)s_catalog_v3.album_count,
         (unsigned long)s_catalog_v3.artist_count,
         (unsigned long)s_catalog_v3.pool.size,
         (int)s_catalog_v3.artist_groups.size(),
         (int)s_catalog_v3.album_groups.size());

    return true;
}

bool storage_catalog_v3_load_or_rebuild(const char* music_root,
                                        const char* v3_index_path)
{
    if (try_load_v3(v3_index_path)) {
        return true;
    }

    LOGW("[曲库] 加载 v3 失败, 回退 native 重建");
    return rebuild_v3_native(music_root, v3_index_path);
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

bool storage_catalog_v3_rebuild(const char* music_root,
                                const char* v3_index_path)
{
    return rebuild_v3_native(music_root, v3_index_path);
}
