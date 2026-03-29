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

  LOGI("[CATALOG_V3] load ok: tracks=%lu albums=%lu artists=%lu pool=%lu artist_groups=%d album_groups=%d",
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

    LOGI("[CATALOG_V3] native rebuild start...");

    if (!storage_scan_music_v3(tmp_tracks, music_root)) {
        LOGE("[CATALOG_V3] native scan failed");
        return false;
    }

    storage_catalog_v3_clear();

    if (!storage_build_catalog_v3_from_temp(tmp_tracks, s_catalog_v3)) {
        LOGE("[CATALOG_V3] build from temp failed");
        storage_catalog_v3_clear();
        return false;
    }

    storage_build_groups_v3(s_catalog_v3);

    s_catalog_v3.generation = ++s_catalog_generation_seq;
    s_v3_ready = true;

    if (!storage_index_save_v3(s_catalog_v3, v3_index_path)) {
        LOGE("[CATALOG_V3] save v3 failed: %s", v3_index_path);
    }

    LOGI("[CATALOG_V3] native rebuild ok: tracks=%lu albums=%lu artists=%lu pool=%lu artist_groups=%d album_groups=%d",
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

    LOGW("[CATALOG_V3] load v3 failed, fallback native rebuild");
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

bool storage_catalog_v3_get_legacy_trackinfo(uint32_t track_index,
                                             TrackInfo& out,
                                             const char* music_root)
{
  if (!storage_catalog_v3_ready()) {
    out = TrackInfo{};
    return false;
  }

  return storage_fill_legacy_trackinfo_from_v3(s_catalog_v3, track_index, out, music_root);
}

void storage_catalog_v3_log_memory_stats(void)
{
  if (!storage_catalog_v3_ready()) {
    LOGE("[CATALOG_V3] memory stats unavailable: catalog not ready");
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

  LOGI("[CATALOG_V3][MEM] sizeof(TrackRowV3)=%u sizeof(AlbumRowV3)=%u sizeof(ArtistRowV3)=%u",
       (unsigned)sizeof(TrackRowV3),
       (unsigned)sizeof(AlbumRowV3),
       (unsigned)sizeof(ArtistRowV3));

  LOGI("[CATALOG_V3][MEM] tracks=%lu -> %lu bytes",
       (unsigned long)s_catalog_v3.track_count,
       (unsigned long)track_bytes);

  LOGI("[CATALOG_V3][MEM] albums=%lu -> %lu bytes",
       (unsigned long)s_catalog_v3.album_count,
       (unsigned long)album_bytes);

  LOGI("[CATALOG_V3][MEM] artists=%lu -> %lu bytes",
       (unsigned long)s_catalog_v3.artist_count,
       (unsigned long)artist_bytes);

  LOGI("[CATALOG_V3][MEM] string_pool=%lu bytes",
       (unsigned long)pool_bytes);

  LOGI("[CATALOG_V3][MEM] artist_groups=%d idx_bytes=%lu",
       (int)s_catalog_v3.artist_groups.size(),
       (unsigned long)groups_artist_idx_bytes);

  LOGI("[CATALOG_V3][MEM] album_groups=%d idx_bytes=%lu",
       (int)s_catalog_v3.album_groups.size(),
       (unsigned long)groups_album_idx_bytes);

  LOGI("[CATALOG_V3][MEM] core_total=%lu bytes (%.2f KB, %.2f MB)",
       (unsigned long)total_core,
       total_core / 1024.0f,
       total_core / 1024.0f / 1024.0f);

  LOGI("[CATALOG_V3][MEM] total_with_groups=%lu bytes (%.2f KB, %.2f MB)",
       (unsigned long)total_with_groups,
       total_with_groups / 1024.0f,
       total_with_groups / 1024.0f / 1024.0f);

  LOGI("[CATALOG_V3][MEM] note: group object/String overhead not fully included");
}

bool storage_catalog_v3_rebuild(const char* music_root,
                                const char* v3_index_path)
{
    return rebuild_v3_native(music_root, v3_index_path);
}
