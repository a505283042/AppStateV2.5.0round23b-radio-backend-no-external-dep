#include "storage/storage_index_v3.h"
#include <SdFat.h>
#include "esp_heap_caps.h"
#include "utils/log.h"
#include "storage/storage_io.h"

extern SdFat sd;

/* ===== 基础 IO ===== */

static bool write_u16(File32& f, uint16_t v) {
  return f.write(&v, sizeof(v)) == sizeof(v);
}

static bool write_u32(File32& f, uint32_t v) {
  return f.write(&v, sizeof(v)) == sizeof(v);
}

static bool read_u16(File32& f, uint16_t& v) {
  return f.read(&v, sizeof(v)) == sizeof(v);
}

static bool read_u32(File32& f, uint32_t& v) {
  return f.read(&v, sizeof(v)) == sizeof(v);
}

static bool write_header(File32& f, const IndexV3Header& h)
{
  return write_u32(f, h.magic) &&
         write_u16(f, h.version) &&
         write_u16(f, h.flags) &&
         write_u32(f, h.header_size) &&
         write_u32(f, h.section_count) &&
         write_u32(f, h.track_count) &&
         write_u32(f, h.album_count) &&
         write_u32(f, h.artist_count) &&
         write_u32(f, h.string_pool_size) &&
         write_u32(f, h.crc32);
}

static bool read_header(File32& f, IndexV3Header& h)
{
  return read_u32(f, h.magic) &&
         read_u16(f, h.version) &&
         read_u16(f, h.flags) &&
         read_u32(f, h.header_size) &&
         read_u32(f, h.section_count) &&
         read_u32(f, h.track_count) &&
         read_u32(f, h.album_count) &&
         read_u32(f, h.artist_count) &&
         read_u32(f, h.string_pool_size) &&
         read_u32(f, h.crc32);
}

static bool write_section(File32& f, const IndexSectionV3& s)
{
  return write_u32(f, s.type) &&
         write_u32(f, s.offset) &&
         write_u32(f, s.size);
}

static bool read_section(File32& f, IndexSectionV3& s)
{
  return read_u32(f, s.type) &&
         read_u32(f, s.offset) &&
         read_u32(f, s.size);
}

/* ===== 内存管理 ===== */

static void* psram_alloc_bytes(size_t n)
{
  if (n == 0) return nullptr;
  return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void* heap_alloc_bytes(size_t n)
{
  if (n == 0) return nullptr;
  return heap_caps_malloc(n, MALLOC_CAP_8BIT);
}

static void* alloc_prefer_psram(size_t n)
{
  void* p = psram_alloc_bytes(n);
  if (p) return p;
  return heap_alloc_bytes(n);
}

void storage_catalog_v3_free(MusicCatalogV3& cat)
{
  if (cat.pool.data) {
    heap_caps_free(cat.pool.data);
    cat.pool.data = nullptr;
  }

  if (cat.tracks) {
    heap_caps_free(cat.tracks);
    cat.tracks = nullptr;
  }

  if (cat.albums) {
    heap_caps_free(cat.albums);
    cat.albums = nullptr;
  }

  if (cat.artists) {
    heap_caps_free(cat.artists);
    cat.artists = nullptr;
  }

  cat.track_count = 0;
  cat.album_count = 0;
  cat.artist_count = 0;
  cat.pool.size = 0;
  cat.clear_runtime_only();
}

/* ===== 保存 ===== */

bool storage_index_save_v3(const MusicCatalogV3& cat, const char* index_path)
{
  StorageSdLockGuard sd_lock(2000);
  if (!sd_lock) {
    LOGE("[INDEX_V3] save lock timeout");
    return false;
  }

  sd.mkdir("/System");

  String tmp_path = String(index_path) + ".tmp";
  File32 f = sd.open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
  if (!f) {
    LOGE("[INDEX_V3] open tmp failed: %s", tmp_path.c_str());
    return false;
  }

  IndexV3Header h;
  h.flags = 0;
  h.section_count = 4;
  h.track_count = cat.track_count;
  h.album_count = cat.album_count;
  h.artist_count = cat.artist_count;
  h.string_pool_size = cat.pool.size;
  h.crc32 = 0;

  const uint32_t header_bytes = sizeof(IndexV3Header);
  const uint32_t section_table_bytes = h.section_count * sizeof(IndexSectionV3);

  uint32_t cur = header_bytes + section_table_bytes;

  IndexSectionV3 sec_pool;
  sec_pool.type = SEC_V3_STR_POOL;
  sec_pool.offset = cur;
  sec_pool.size = cat.pool.size;
  cur += sec_pool.size;

  IndexSectionV3 sec_artists;
  sec_artists.type = SEC_V3_ARTISTS;
  sec_artists.offset = cur;
  sec_artists.size = cat.artist_count * sizeof(ArtistRowV3);
  cur += sec_artists.size;

  IndexSectionV3 sec_albums;
  sec_albums.type = SEC_V3_ALBUMS;
  sec_albums.offset = cur;
  sec_albums.size = cat.album_count * sizeof(AlbumRowV3);
  cur += sec_albums.size;

  IndexSectionV3 sec_tracks;
  sec_tracks.type = SEC_V3_TRACKS;
  sec_tracks.offset = cur;
  sec_tracks.size = cat.track_count * sizeof(TrackRowV3);
  cur += sec_tracks.size;

  if (!write_header(f, h) ||
      !write_section(f, sec_pool) ||
      !write_section(f, sec_artists) ||
      !write_section(f, sec_albums) ||
      !write_section(f, sec_tracks)) {
    f.close();
    sd.remove(tmp_path.c_str());
    LOGE("[INDEX_V3] write header/sections failed");
    return false;
  }

  if (cat.pool.size > 0) {
    if (!cat.pool.data || f.write(cat.pool.data, cat.pool.size) != (int)cat.pool.size) {
      f.close();
      sd.remove(tmp_path.c_str());
      LOGE("[INDEX_V3] write string pool failed");
      return false;
    }
  }

  if (cat.artist_count > 0) {
    size_t n = cat.artist_count * sizeof(ArtistRowV3);
    if (!cat.artists || f.write((const uint8_t*)cat.artists, n) != (int)n) {
      f.close();
      sd.remove(tmp_path.c_str());
      LOGE("[INDEX_V3] write artists failed");
      return false;
    }
  }

  if (cat.album_count > 0) {
    size_t n = cat.album_count * sizeof(AlbumRowV3);
    if (!cat.albums || f.write((const uint8_t*)cat.albums, n) != (int)n) {
      f.close();
      sd.remove(tmp_path.c_str());
      LOGE("[INDEX_V3] write albums failed");
      return false;
    }
  }

  if (cat.track_count > 0) {
    size_t n = cat.track_count * sizeof(TrackRowV3);
    if (!cat.tracks || f.write((const uint8_t*)cat.tracks, n) != (int)n) {
      f.close();
      sd.remove(tmp_path.c_str());
      LOGE("[INDEX_V3] write tracks failed");
      return false;
    }
  }

  f.close();

  sd.remove(index_path);
  if (!sd.rename(tmp_path.c_str(), index_path)) {
    sd.remove(tmp_path.c_str());
    LOGE("[INDEX_V3] rename tmp -> final failed");
    return false;
  }

  LOGI("[INDEX_V3] save ok: %s tracks=%lu albums=%lu artists=%lu pool=%lu",
       index_path,
       (unsigned long)cat.track_count,
       (unsigned long)cat.album_count,
       (unsigned long)cat.artist_count,
       (unsigned long)cat.pool.size);
  return true;
}

/* ===== 加载 ===== */

static bool load_section_blob(File32& f, const IndexSectionV3& s, void* dst, size_t expect_size)
{
  if (s.size != expect_size) return false;
  if (!f.seekSet(s.offset)) return false;
  return f.read((uint8_t*)dst, expect_size) == (int)expect_size;
}

bool storage_index_load_v3(MusicCatalogV3& out_cat, const char* index_path)
{
  storage_catalog_v3_free(out_cat);

  StorageSdLockGuard sd_lock(2000);
  if (!sd_lock) {
    LOGE("[INDEX_V3] load lock timeout");
    return false;
  }

  File32 f = sd.open(index_path, O_RDONLY);
  if (!f) {
    LOGE("[INDEX_V3] open failed: %s", index_path);
    return false;
  }

  IndexV3Header h;
  if (!read_header(f, h)) {
    f.close();
    LOGE("[INDEX_V3] read header failed");
    return false;
  }

  if (h.magic != INDEX_V3_MAGIC || h.version != INDEX_V3_VERSION) {
    f.close();
    LOGE("[INDEX_V3] unsupported format magic=0x%08lx ver=%u",
         (unsigned long)h.magic, (unsigned)h.version);
    return false;
  }

  if (h.section_count != 4 || h.track_count > 100000 || h.album_count > 20000 || h.artist_count > 20000) {
    f.close();
    LOGE("[INDEX_V3] unreasonable counts");
    return false;
  }

  IndexSectionV3 sections[4];
  for (uint32_t i = 0; i < 4; ++i) {
    if (!read_section(f, sections[i])) {
      f.close();
      LOGE("[INDEX_V3] read section failed");
      return false;
    }
  }

  IndexSectionV3 sec_pool = {};
  IndexSectionV3 sec_artists = {};
  IndexSectionV3 sec_albums = {};
  IndexSectionV3 sec_tracks = {};

  for (uint32_t i = 0; i < 4; ++i) {
    switch (sections[i].type) {
      case SEC_V3_STR_POOL: sec_pool = sections[i]; break;
      case SEC_V3_ARTISTS:  sec_artists = sections[i]; break;
      case SEC_V3_ALBUMS:   sec_albums = sections[i]; break;
      case SEC_V3_TRACKS:   sec_tracks = sections[i]; break;
      default:
        f.close();
        LOGE("[INDEX_V3] unknown section type=%lu", (unsigned long)sections[i].type);
        return false;
    }
  }

  if (h.string_pool_size > 0) {
    out_cat.pool.data = (uint8_t*)alloc_prefer_psram(h.string_pool_size);
    if (!out_cat.pool.data) {
      f.close();
      storage_catalog_v3_free(out_cat);
      LOGE("[INDEX_V3] alloc pool failed");
      return false;
    }
    out_cat.pool.size = h.string_pool_size;
    if (!load_section_blob(f, sec_pool, out_cat.pool.data, h.string_pool_size)) {
      f.close();
      storage_catalog_v3_free(out_cat);
      LOGE("[INDEX_V3] load pool failed");
      return false;
    }
  }

  if (h.artist_count > 0) {
    size_t n = h.artist_count * sizeof(ArtistRowV3);
    out_cat.artists = (ArtistRowV3*)alloc_prefer_psram(n);
    if (!out_cat.artists) {
      f.close();
      storage_catalog_v3_free(out_cat);
      LOGE("[INDEX_V3] alloc artists failed");
      return false;
    }
    if (!load_section_blob(f, sec_artists, out_cat.artists, n)) {
      f.close();
      storage_catalog_v3_free(out_cat);
      LOGE("[INDEX_V3] load artists failed");
      return false;
    }
    out_cat.artist_count = h.artist_count;
  }

  if (h.album_count > 0) {
    size_t n = h.album_count * sizeof(AlbumRowV3);
    out_cat.albums = (AlbumRowV3*)alloc_prefer_psram(n);
    if (!out_cat.albums) {
      f.close();
      storage_catalog_v3_free(out_cat);
      LOGE("[INDEX_V3] alloc albums failed");
      return false;
    }
    if (!load_section_blob(f, sec_albums, out_cat.albums, n)) {
      f.close();
      storage_catalog_v3_free(out_cat);
      LOGE("[INDEX_V3] load albums failed");
      return false;
    }
    out_cat.album_count = h.album_count;
  }

  if (h.track_count > 0) {
    size_t n = h.track_count * sizeof(TrackRowV3);
    out_cat.tracks = (TrackRowV3*)alloc_prefer_psram(n);
    if (!out_cat.tracks) {
      f.close();
      storage_catalog_v3_free(out_cat);
      LOGE("[INDEX_V3] alloc tracks failed");
      return false;
    }
    if (!load_section_blob(f, sec_tracks, out_cat.tracks, n)) {
      f.close();
      storage_catalog_v3_free(out_cat);
      LOGE("[INDEX_V3] load tracks failed");
      return false;
    }
    out_cat.track_count = h.track_count;
  }

  f.close();

  LOGI("[INDEX_V3] load ok: %s tracks=%lu albums=%lu artists=%lu pool=%lu",
       index_path,
       (unsigned long)out_cat.track_count,
       (unsigned long)out_cat.album_count,
       (unsigned long)out_cat.artist_count,
       (unsigned long)out_cat.pool.size);
  return true;
}