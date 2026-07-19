#include "storage/storage_index_v3.h"

#include <SdFat.h>
#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <limits>

#include "esp_heap_caps.h"
#include "storage/storage_io.h"
#include "storage/system_paths.h"
#include "utils/log.h"

extern SdFat sd;

namespace {

static constexpr uint32_t kIndexHeaderBytes = 36;
static constexpr uint32_t kIndexSectionBytes = 12;
static constexpr uint32_t kRequiredSectionCount = 4;
static constexpr uint32_t kMaxTrackCount = UINT16_MAX;
static constexpr uint32_t kMaxAlbumCount = 20000;
static constexpr uint32_t kMaxArtistCount = 20000;
static constexpr uint32_t kMaxStringPoolBytes = 16u * 1024u * 1024u;
static constexpr uint32_t kMaxIndexFileBytes = 32u * 1024u * 1024u;
static constexpr size_t kInternalFallbackMaxBytes = 32u * 1024u;

static_assert(sizeof(IndexV3Header) == kIndexHeaderBytes,
              "IndexV3Header 布局变化会破坏磁盘格式");
static_assert(sizeof(IndexSectionV3) == kIndexSectionBytes,
              "IndexSectionV3 布局变化会破坏磁盘格式");

struct IndexLayoutV3 {
  IndexV3Header header{};
  IndexSectionV3 pool{};
  IndexSectionV3 artists{};
  IndexSectionV3 albums{};
  IndexSectionV3 tracks{};
  uint32_t file_size = 0;
  bool has_crc = false;
};

enum class CandidateState : uint8_t {
  Missing = 0,
  Valid,
  Invalid,
  Unavailable,
};

enum class LayoutValidationState : uint8_t {
  Valid = 0,
  Corrupt,
  IoError,
};

static bool s_last_load_needs_rewrite = false;
static const char* s_last_load_source = "none";

static void set_reason(char* out, size_t out_size, const char* fmt, ...)
{
  if (!out || out_size == 0) return;

  va_list ap;
  va_start(ap, fmt);
  vsnprintf(out, out_size, fmt, ap);
  va_end(ap);
  out[out_size - 1] = '\0';
}

/* ===== 基础 IO ===== */

static bool write_u16(File32& f, uint16_t v)
{
  return f.write(&v, sizeof(v)) == sizeof(v);
}

static bool write_u32(File32& f, uint32_t v)
{
  return f.write(&v, sizeof(v)) == sizeof(v);
}

static bool read_u16(File32& f, uint16_t& v)
{
  return f.read(&v, sizeof(v)) == (int)sizeof(v);
}

static bool read_u32(File32& f, uint32_t& v)
{
  return f.read(&v, sizeof(v)) == (int)sizeof(v);
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

/* ===== CRC32 ===== */

static uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t size)
{
  if (!data || size == 0) return crc;

  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      const uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return crc;
}

static uint32_t crc32_update_u32_le(uint32_t crc, uint32_t value)
{
  uint8_t bytes[4] = {
      (uint8_t)(value & 0xFFu),
      (uint8_t)((value >> 8) & 0xFFu),
      (uint8_t)((value >> 16) & 0xFFu),
      (uint8_t)((value >> 24) & 0xFFu),
  };
  return crc32_update(crc, bytes, sizeof(bytes));
}

static uint32_t crc32_update_section(uint32_t crc, const IndexSectionV3& section)
{
  crc = crc32_update_u32_le(crc, section.type);
  crc = crc32_update_u32_le(crc, section.offset);
  crc = crc32_update_u32_le(crc, section.size);
  return crc;
}

static uint32_t calculate_catalog_crc(const MusicCatalogV3& cat,
                                      const IndexSectionV3 sections[kRequiredSectionCount])
{
  uint32_t crc = 0xFFFFFFFFu;

  for (uint32_t i = 0; i < kRequiredSectionCount; ++i) {
    crc = crc32_update_section(crc, sections[i]);
  }

  crc = crc32_update(crc, cat.pool.data, cat.pool.size);
  crc = crc32_update(crc,
                     reinterpret_cast<const uint8_t*>(cat.artists),
                     (size_t)cat.artist_count * sizeof(ArtistRowV3));
  crc = crc32_update(crc,
                     reinterpret_cast<const uint8_t*>(cat.albums),
                     (size_t)cat.album_count * sizeof(AlbumRowV3));
  crc = crc32_update(crc,
                     reinterpret_cast<const uint8_t*>(cat.tracks),
                     (size_t)cat.track_count * sizeof(TrackRowV3));

  return crc ^ 0xFFFFFFFFu;
}

static bool calculate_file_crc(File32& f,
                               uint32_t offset,
                               uint32_t size,
                               uint32_t& out_crc)
{
  out_crc = 0;
  if (!f.seekSet(offset)) return false;

  uint8_t buffer[512];
  uint32_t remaining = size;
  uint32_t crc = 0xFFFFFFFFu;

  while (remaining > 0) {
    const size_t chunk = std::min<size_t>(sizeof(buffer), remaining);
    const int got = f.read(buffer, chunk);
    if (got != (int)chunk) {
      return false;
    }
    crc = crc32_update(crc, buffer, chunk);
    remaining -= (uint32_t)chunk;
  }

  out_crc = crc ^ 0xFFFFFFFFu;
  return true;
}

/* ===== 内存管理 ===== */

static void* psram_alloc_bytes(size_t size)
{
  if (size == 0) return nullptr;
  return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void* heap_alloc_bytes(size_t size)
{
  if (size == 0) return nullptr;
  return heap_caps_malloc(size, MALLOC_CAP_8BIT);
}

static void* alloc_prefer_psram(size_t size)
{
  void* ptr = psram_alloc_bytes(size);
  if (ptr) return ptr;

  if (size > kInternalFallbackMaxBytes) {
    LOGW("[曲库索引] 大缓冲 PSRAM 分配失败，禁止回落内部RAM size=%lu",
         (unsigned long)size);
    return nullptr;
  }

  return heap_alloc_bytes(size);
}

/* ===== 路径与文件操作 ===== */

static bool path_exists_locked(const char* path)
{
  if (!path || !path[0]) return false;
  File32 f = sd.open(path, O_RDONLY);
  const bool exists = (bool)f;
  if (f) f.close();
  return exists;
}

static void remove_if_exists_locked(const char* path)
{
  if (path_exists_locked(path)) {
    sd.remove(path);
  }
}

static bool quarantine_file_locked(const char* path, const char* bad_path)
{
  if (!path_exists_locked(path)) return true;

  remove_if_exists_locked(bad_path);
  if (!sd.rename(path, bad_path)) {
    LOGW("[曲库索引] 无法隔离异常文件：%s -> %s", path, bad_path);
    return false;
  }

  LOGW("[曲库索引] 异常文件已隔离：%s", bad_path);
  return true;
}

/* ===== 结构完整性 ===== */

static bool multiply_u32_checked(uint32_t count,
                                 size_t item_size,
                                 uint32_t& out_size)
{
  const uint64_t total = (uint64_t)count * (uint64_t)item_size;
  if (total > UINT32_MAX) return false;
  out_size = (uint32_t)total;
  return true;
}

static bool ranges_overlap(const IndexSectionV3& a, const IndexSectionV3& b)
{
  if (a.size == 0 || b.size == 0) return false;
  const uint64_t a_end = (uint64_t)a.offset + a.size;
  const uint64_t b_end = (uint64_t)b.offset + b.size;
  return (uint64_t)a.offset < b_end && (uint64_t)b.offset < a_end;
}

static LayoutValidationState validate_file_layout(File32& f,
                                                  const char* path,
                                                  IndexLayoutV3& out,
                                                  char* reason,
                                                  size_t reason_size)
{
  out = IndexLayoutV3{};

  const uint64_t file_size64 = f.fileSize();
  const uint32_t table_end =
      kIndexHeaderBytes + kRequiredSectionCount * kIndexSectionBytes;

  if (file_size64 < table_end || file_size64 > kMaxIndexFileBytes) {
    set_reason(reason, reason_size,
               "文件长度异常 size=%llu",
               (unsigned long long)file_size64);
    return LayoutValidationState::Corrupt;
  }
  out.file_size = (uint32_t)file_size64;

  if (!f.seekSet(0) || !read_header(f, out.header)) {
    set_reason(reason, reason_size, "文件头读取失败");
    return LayoutValidationState::IoError;
  }

  const IndexV3Header& h = out.header;
  if (h.magic != INDEX_V3_MAGIC) {
    set_reason(reason, reason_size,
               "魔数错误 0x%08lx",
               (unsigned long)h.magic);
    return LayoutValidationState::Corrupt;
  }
  if (h.version != INDEX_V3_VERSION) {
    set_reason(reason, reason_size, "版本不支持 %u", (unsigned)h.version);
    return LayoutValidationState::Corrupt;
  }
  if (h.header_size != kIndexHeaderBytes) {
    set_reason(reason, reason_size,
               "文件头长度错误 %lu",
               (unsigned long)h.header_size);
    return LayoutValidationState::Corrupt;
  }
  if (h.section_count != kRequiredSectionCount) {
    set_reason(reason, reason_size,
               "区段数量错误 %lu",
               (unsigned long)h.section_count);
    return LayoutValidationState::Corrupt;
  }
  if ((h.flags & ~INDEX_V3_FLAG_CRC32) != 0) {
    set_reason(reason, reason_size,
               "未知标志 0x%04X",
               (unsigned)h.flags);
    return LayoutValidationState::Corrupt;
  }
  if (h.track_count == 0 || h.track_count > kMaxTrackCount ||
      h.album_count > kMaxAlbumCount ||
      h.artist_count > kMaxArtistCount ||
      h.string_pool_size == 0 ||
      h.string_pool_size > kMaxStringPoolBytes) {
    set_reason(reason, reason_size,
               "计数异常 tracks=%lu albums=%lu artists=%lu pool=%lu",
               (unsigned long)h.track_count,
               (unsigned long)h.album_count,
               (unsigned long)h.artist_count,
               (unsigned long)h.string_pool_size);
    return LayoutValidationState::Corrupt;
  }

  IndexSectionV3 sections[kRequiredSectionCount]{};
  for (uint32_t i = 0; i < kRequiredSectionCount; ++i) {
    if (!read_section(f, sections[i])) {
      set_reason(reason, reason_size, "区段表读取失败 index=%lu", (unsigned long)i);
      return LayoutValidationState::IoError;
    }
  }

  bool found_pool = false;
  bool found_artists = false;
  bool found_albums = false;
  bool found_tracks = false;

  for (uint32_t i = 0; i < kRequiredSectionCount; ++i) {
    const IndexSectionV3& section = sections[i];
    switch (section.type) {
      case SEC_V3_STR_POOL:
        if (found_pool) {
          set_reason(reason, reason_size, "字符串池区段重复");
          return LayoutValidationState::Corrupt;
        }
        found_pool = true;
        out.pool = section;
        break;

      case SEC_V3_ARTISTS:
        if (found_artists) {
          set_reason(reason, reason_size, "歌手区段重复");
          return LayoutValidationState::Corrupt;
        }
        found_artists = true;
        out.artists = section;
        break;

      case SEC_V3_ALBUMS:
        if (found_albums) {
          set_reason(reason, reason_size, "专辑区段重复");
          return LayoutValidationState::Corrupt;
        }
        found_albums = true;
        out.albums = section;
        break;

      case SEC_V3_TRACKS:
        if (found_tracks) {
          set_reason(reason, reason_size, "歌曲区段重复");
          return LayoutValidationState::Corrupt;
        }
        found_tracks = true;
        out.tracks = section;
        break;

      default:
        set_reason(reason, reason_size,
                   "未知区段 type=%lu",
                   (unsigned long)section.type);
        return LayoutValidationState::Corrupt;
    }
  }

  if (!found_pool || !found_artists || !found_albums || !found_tracks) {
    set_reason(reason, reason_size, "缺少必要区段");
    return LayoutValidationState::Corrupt;
  }

  uint32_t expected_artists = 0;
  uint32_t expected_albums = 0;
  uint32_t expected_tracks = 0;
  if (!multiply_u32_checked(h.artist_count, sizeof(ArtistRowV3), expected_artists) ||
      !multiply_u32_checked(h.album_count, sizeof(AlbumRowV3), expected_albums) ||
      !multiply_u32_checked(h.track_count, sizeof(TrackRowV3), expected_tracks)) {
    set_reason(reason, reason_size, "区段长度计算溢出");
    return LayoutValidationState::Corrupt;
  }

  if (out.pool.size != h.string_pool_size ||
      out.artists.size != expected_artists ||
      out.albums.size != expected_albums ||
      out.tracks.size != expected_tracks) {
    set_reason(reason, reason_size,
               "区段长度不匹配 pool=%lu/%lu artists=%lu/%lu albums=%lu/%lu tracks=%lu/%lu",
               (unsigned long)out.pool.size,
               (unsigned long)h.string_pool_size,
               (unsigned long)out.artists.size,
               (unsigned long)expected_artists,
               (unsigned long)out.albums.size,
               (unsigned long)expected_albums,
               (unsigned long)out.tracks.size,
               (unsigned long)expected_tracks);
    return LayoutValidationState::Corrupt;
  }

  const IndexSectionV3 ordered[kRequiredSectionCount] = {
      out.pool, out.artists, out.albums, out.tracks,
  };

  uint64_t payload_sum = 0;
  uint64_t max_end = table_end;
  for (uint32_t i = 0; i < kRequiredSectionCount; ++i) {
    const IndexSectionV3& section = ordered[i];
    if (section.offset < table_end) {
      set_reason(reason, reason_size,
                 "区段越过文件头 type=%lu offset=%lu",
                 (unsigned long)section.type,
                 (unsigned long)section.offset);
      return LayoutValidationState::Corrupt;
    }

    const uint64_t end = (uint64_t)section.offset + section.size;
    if (end > file_size64 || end < section.offset) {
      set_reason(reason, reason_size,
                 "区段越界 type=%lu offset=%lu size=%lu",
                 (unsigned long)section.type,
                 (unsigned long)section.offset,
                 (unsigned long)section.size);
      return LayoutValidationState::Corrupt;
    }

    payload_sum += section.size;
    if (end > max_end) max_end = end;
  }

  for (uint32_t i = 0; i < kRequiredSectionCount; ++i) {
    for (uint32_t j = i + 1; j < kRequiredSectionCount; ++j) {
      if (ranges_overlap(ordered[i], ordered[j])) {
        set_reason(reason, reason_size,
                   "区段重叠 type=%lu/%lu",
                   (unsigned long)ordered[i].type,
                   (unsigned long)ordered[j].type);
        return LayoutValidationState::Corrupt;
      }
    }
  }

  if ((uint64_t)table_end + payload_sum != file_size64 || max_end != file_size64) {
    set_reason(reason, reason_size,
               "存在区段空洞或尾部垃圾 file=%llu expected=%llu max_end=%llu",
               (unsigned long long)file_size64,
               (unsigned long long)((uint64_t)table_end + payload_sum),
               (unsigned long long)max_end);
    return LayoutValidationState::Corrupt;
  }

  out.has_crc = ((h.flags & INDEX_V3_FLAG_CRC32) != 0) || h.crc32 != 0;
  if (out.has_crc) {
    uint32_t actual_crc = 0;
    if (!calculate_file_crc(f,
                            kIndexHeaderBytes,
                            out.file_size - kIndexHeaderBytes,
                            actual_crc)) {
      set_reason(reason, reason_size, "CRC读取失败");
      return LayoutValidationState::IoError;
    }

    if (actual_crc != h.crc32) {
      set_reason(reason, reason_size,
                 "CRC不匹配 file=0x%08lx calc=0x%08lx",
                 (unsigned long)h.crc32,
                 (unsigned long)actual_crc);
      return LayoutValidationState::Corrupt;
    }
  }

  (void)path;
  return LayoutValidationState::Valid;
}

/* ===== 语义完整性 ===== */

static bool validate_string_offset(const StringPoolV3& pool,
                                   uint32_t offset,
                                   bool required,
                                   const char* field,
                                   uint32_t row,
                                   char* reason,
                                   size_t reason_size)
{
  if (offset == INVALID_OFF32) {
    if (required) {
      set_reason(reason, reason_size,
                 "%s[%lu] 必填字符串为空",
                 field,
                 (unsigned long)row);
      return false;
    }
    return true;
  }

  if (!pool.data || offset >= pool.size) {
    set_reason(reason, reason_size,
               "%s[%lu] 字符串偏移越界 off=%lu pool=%lu",
               field,
               (unsigned long)row,
               (unsigned long)offset,
               (unsigned long)pool.size);
    return false;
  }

  if (offset > 0 && pool.data[offset - 1] != 0) {
    set_reason(reason, reason_size,
               "%s[%lu] 偏移不是字符串起点 off=%lu",
               field,
               (unsigned long)row,
               (unsigned long)offset);
    return false;
  }

  if (!memchr(pool.data + offset, 0, pool.size - offset)) {
    set_reason(reason, reason_size,
               "%s[%lu] 字符串缺少结束符 off=%lu",
               field,
               (unsigned long)row,
               (unsigned long)offset);
    return false;
  }

  if (required && pool.data[offset] == 0) {
    set_reason(reason, reason_size,
               "%s[%lu] 必填字符串为空串",
               field,
               (unsigned long)row);
    return false;
  }

  return true;
}

static bool validate_catalog_semantics(const MusicCatalogV3& cat,
                                       char* reason,
                                       size_t reason_size)
{
  if (!cat.pool.data || cat.pool.size == 0 ||
      cat.pool.data[0] != 0 || cat.pool.data[cat.pool.size - 1] != 0) {
    set_reason(reason, reason_size, "字符串池首尾哨兵无效");
    return false;
  }

  if (!cat.tracks || cat.track_count == 0 || cat.track_count > kMaxTrackCount) {
    set_reason(reason, reason_size,
               "歌曲表无效 count=%lu",
               (unsigned long)cat.track_count);
    return false;
  }
  if (cat.album_count > 0 && !cat.albums) {
    set_reason(reason, reason_size, "专辑表指针为空");
    return false;
  }
  if (cat.artist_count > 0 && !cat.artists) {
    set_reason(reason, reason_size, "歌手表指针为空");
    return false;
  }

  for (uint32_t i = 0; i < cat.artist_count; ++i) {
    if (!validate_string_offset(cat.pool,
                                cat.artists[i].name_off,
                                true,
                                "artist.name",
                                i,
                                reason,
                                reason_size)) {
      return false;
    }
  }

  for (uint32_t i = 0; i < cat.album_count; ++i) {
    const AlbumRowV3& album = cat.albums[i];
    if (!validate_string_offset(cat.pool, album.name_off, true,
                                "album.name", i, reason, reason_size) ||
        !validate_string_offset(cat.pool, album.primary_artist_off, true,
                                "album.artist", i, reason, reason_size) ||
        !validate_string_offset(cat.pool, album.folder_cover_off, false,
                                "album.cover", i, reason, reason_size)) {
      return false;
    }
  }

  static constexpr uint16_t kKnownTrackFlags =
      TF_HAS_LRC | TF_HAS_EMBED_COVER | TF_HAS_FILE_COVER |
      TF_IS_MP3 | TF_IS_FLAC;

  for (uint32_t i = 0; i < cat.track_count; ++i) {
    const TrackRowV3& track = cat.tracks[i];

    if (!validate_string_offset(cat.pool, track.title_off, true,
                                "track.title", i, reason, reason_size) ||
        !validate_string_offset(cat.pool, track.artist_off, false,
                                "track.artist", i, reason, reason_size) ||
        !validate_string_offset(cat.pool, track.audio_rel_off, true,
                                "track.audio", i, reason, reason_size) ||
        !validate_string_offset(cat.pool, track.lrc_rel_off, false,
                                "track.lrc", i, reason, reason_size) ||
        !validate_string_offset(cat.pool, track.cover_path_off, false,
                                "track.cover", i, reason, reason_size) ||
        !validate_string_offset(cat.pool, track.mime_off, false,
                                "track.mime", i, reason, reason_size)) {
      return false;
    }

    if (track.album_id == INVALID_ID32 || track.album_id >= cat.album_count) {
      set_reason(reason, reason_size,
                 "track.album[%lu] 越界 id=%lu count=%lu",
                 (unsigned long)i,
                 (unsigned long)track.album_id,
                 (unsigned long)cat.album_count);
      return false;
    }

    if (track.cover_source > COVER_FILE_FALLBACK) {
      set_reason(reason, reason_size,
                 "track.cover_source[%lu] 无效 value=%u",
                 (unsigned long)i,
                 (unsigned)track.cover_source);
      return false;
    }

    if (track.ext_code != EXT_MP3 && track.ext_code != EXT_FLAC) {
      set_reason(reason, reason_size,
                 "track.ext[%lu] 无效 value=%u",
                 (unsigned long)i,
                 (unsigned)track.ext_code);
      return false;
    }

    if ((track.flags & ~kKnownTrackFlags) != 0) {
      set_reason(reason, reason_size,
                 "track.flags[%lu] 未知位=0x%04X",
                 (unsigned long)i,
                 (unsigned)(track.flags & ~kKnownTrackFlags));
      return false;
    }

    const bool mp3_flags_ok =
        track.ext_code != EXT_MP3 ||
        ((track.flags & TF_IS_MP3) != 0 && (track.flags & TF_IS_FLAC) == 0);
    const bool flac_flags_ok =
        track.ext_code != EXT_FLAC ||
        ((track.flags & TF_IS_FLAC) != 0 && (track.flags & TF_IS_MP3) == 0);
    if (!mp3_flags_ok || !flac_flags_ok) {
      set_reason(reason, reason_size,
                 "track.ext_flags[%lu] 不一致 ext=%u flags=0x%04X",
                 (unsigned long)i,
                 (unsigned)track.ext_code,
                 (unsigned)track.flags);
      return false;
    }

    if ((track.cover_source == COVER_MP3_APIC ||
         track.cover_source == COVER_FLAC_PICTURE)) {
      if (track.cover_size == 0 || (track.flags & TF_HAS_EMBED_COVER) == 0) {
        set_reason(reason, reason_size,
                   "track.embed_cover[%lu] 信息不完整 offset=%lu size=%lu flags=0x%04X",
                   (unsigned long)i,
                   (unsigned long)track.cover_offset,
                   (unsigned long)track.cover_size,
                   (unsigned)track.flags);
        return false;
      }
      if ((uint64_t)track.cover_offset + track.cover_size > UINT32_MAX) {
        set_reason(reason, reason_size,
                   "track.embed_cover[%lu] 范围溢出",
                   (unsigned long)i);
        return false;
      }
    }

    if (track.cover_source == COVER_FILE_FALLBACK) {
      if (track.cover_path_off == INVALID_OFF32 ||
          (track.flags & TF_HAS_FILE_COVER) == 0) {
        set_reason(reason, reason_size,
                   "track.file_cover[%lu] 信息不完整",
                   (unsigned long)i);
        return false;
      }
    }
  }

  return true;
}

static bool load_section_blob(File32& f,
                              const IndexSectionV3& section,
                              void* destination,
                              size_t expected_size)
{
  if (section.size != expected_size) return false;
  if (expected_size == 0) return true;
  if (!destination || !f.seekSet(section.offset)) return false;
  return f.read(reinterpret_cast<uint8_t*>(destination), expected_size) ==
         (int)expected_size;
}

static CandidateState load_candidate_locked(const char* path,
                                            MusicCatalogV3& out_cat,
                                            bool& out_legacy_crc,
                                            char* reason,
                                            size_t reason_size)
{
  out_legacy_crc = false;
  storage_catalog_v3_free(out_cat);

  File32 f = sd.open(path, O_RDONLY);
  if (!f) {
    set_reason(reason, reason_size, "文件不存在");
    return CandidateState::Missing;
  }

  IndexLayoutV3 layout;
  const LayoutValidationState layout_state =
      validate_file_layout(f, path, layout, reason, reason_size);
  if (layout_state != LayoutValidationState::Valid) {
    f.close();
    return layout_state == LayoutValidationState::IoError
        ? CandidateState::Unavailable
        : CandidateState::Invalid;
  }

  const IndexV3Header& h = layout.header;

  out_cat.pool.data = static_cast<uint8_t*>(alloc_prefer_psram(h.string_pool_size));
  if (!out_cat.pool.data) {
    f.close();
    set_reason(reason, reason_size,
               "字符串池分配失败 size=%lu",
               (unsigned long)h.string_pool_size);
    return CandidateState::Unavailable;
  }
  out_cat.pool.size = h.string_pool_size;

  if (h.artist_count > 0) {
    const size_t bytes = (size_t)h.artist_count * sizeof(ArtistRowV3);
    out_cat.artists = static_cast<ArtistRowV3*>(alloc_prefer_psram(bytes));
    if (!out_cat.artists) {
      f.close();
      storage_catalog_v3_free(out_cat);
      set_reason(reason, reason_size,
                 "歌手表分配失败 size=%lu",
                 (unsigned long)bytes);
      return CandidateState::Unavailable;
    }
  }

  if (h.album_count > 0) {
    const size_t bytes = (size_t)h.album_count * sizeof(AlbumRowV3);
    out_cat.albums = static_cast<AlbumRowV3*>(alloc_prefer_psram(bytes));
    if (!out_cat.albums) {
      f.close();
      storage_catalog_v3_free(out_cat);
      set_reason(reason, reason_size,
                 "专辑表分配失败 size=%lu",
                 (unsigned long)bytes);
      return CandidateState::Unavailable;
    }
  }

  const size_t track_bytes = (size_t)h.track_count * sizeof(TrackRowV3);
  out_cat.tracks = static_cast<TrackRowV3*>(alloc_prefer_psram(track_bytes));
  if (!out_cat.tracks) {
    f.close();
    storage_catalog_v3_free(out_cat);
    set_reason(reason, reason_size,
               "歌曲表分配失败 size=%lu",
               (unsigned long)track_bytes);
    return CandidateState::Unavailable;
  }

  if (!load_section_blob(f, layout.pool,
                         out_cat.pool.data, out_cat.pool.size) ||
      !load_section_blob(f, layout.artists,
                         out_cat.artists,
                         (size_t)h.artist_count * sizeof(ArtistRowV3)) ||
      !load_section_blob(f, layout.albums,
                         out_cat.albums,
                         (size_t)h.album_count * sizeof(AlbumRowV3)) ||
      !load_section_blob(f, layout.tracks,
                         out_cat.tracks,
                         track_bytes)) {
    f.close();
    storage_catalog_v3_free(out_cat);
    set_reason(reason, reason_size, "区段内容读取失败");
    return CandidateState::Unavailable;
  }

  f.close();

  out_cat.track_count = h.track_count;
  out_cat.album_count = h.album_count;
  out_cat.artist_count = h.artist_count;

  if (!validate_catalog_semantics(out_cat, reason, reason_size)) {
    storage_catalog_v3_free(out_cat);
    return CandidateState::Invalid;
  }

  out_legacy_crc = !layout.has_crc;

  LOGI("[曲库索引] 完整性校验通过：%s size=%lu CRC=%s 歌曲=%lu 专辑=%lu 歌手=%lu 字符池=%lu",
       path,
       (unsigned long)layout.file_size,
       layout.has_crc ? "通过" : "旧版无CRC",
       (unsigned long)out_cat.track_count,
       (unsigned long)out_cat.album_count,
       (unsigned long)out_cat.artist_count,
       (unsigned long)out_cat.pool.size);

  return CandidateState::Valid;
}

static bool promote_candidate_locked(const char* candidate_path,
                                     const char* final_path,
                                     const char* bad_path)
{
  if (strcmp(candidate_path, final_path) == 0) return true;

  if (path_exists_locked(final_path)) {
    if (!quarantine_file_locked(final_path, bad_path)) {
      return false;
    }
  }

  if (!sd.rename(candidate_path, final_path)) {
    LOGE("[曲库索引] 恢复文件提升失败：%s -> %s",
         candidate_path,
         final_path);
    return false;
  }

  LOGW("[曲库索引] 已恢复正式索引：%s", final_path);
  return true;
}

static bool write_blob(File32& f, const void* data, size_t size)
{
  if (size == 0) return true;
  if (!data) return false;
  return f.write(reinterpret_cast<const uint8_t*>(data), size) == size;
}

}  // namespace

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

bool storage_index_save_v3(const MusicCatalogV3& cat, const char* index_path)
{
  char reason[192] = {};
  if (!validate_catalog_semantics(cat, reason, sizeof(reason))) {
    LOGE("[曲库索引] 拒绝保存无效目录：%s", reason);
    return false;
  }

  StorageSdLockGuard sd_lock(2000);
  if (!sd_lock) {
    LOGE("[曲库索引] 保存锁超时");
    return false;
  }

  sd.mkdir(SystemPaths::kRoot);
  sd.mkdir(SystemPaths::kLibraryDir);

  const String final_path(index_path);
  const String tmp_path = final_path + ".tmp";
  const String backup_path = final_path + ".bak";
  const String bad_path = final_path + ".bad";

  remove_if_exists_locked(tmp_path.c_str());

  IndexV3Header header;
  header.flags = INDEX_V3_FLAG_CRC32;
  header.header_size = kIndexHeaderBytes;
  header.section_count = kRequiredSectionCount;
  header.track_count = cat.track_count;
  header.album_count = cat.album_count;
  header.artist_count = cat.artist_count;
  header.string_pool_size = cat.pool.size;

  uint32_t cursor =
      kIndexHeaderBytes + kRequiredSectionCount * kIndexSectionBytes;

  IndexSectionV3 sections[kRequiredSectionCount]{};

  // 兼容当前 ESP32 工具链：带默认成员初始化的结构体不能直接使用
  // 花括号列表给已存在的数组元素赋值，因此逐字段填写区段信息。
  sections[0].type = SEC_V3_STR_POOL;
  sections[0].offset = cursor;
  sections[0].size = cat.pool.size;
  cursor += sections[0].size;

  sections[1].type = SEC_V3_ARTISTS;
  sections[1].offset = cursor;
  sections[1].size =
      (uint32_t)((size_t)cat.artist_count * sizeof(ArtistRowV3));
  cursor += sections[1].size;

  sections[2].type = SEC_V3_ALBUMS;
  sections[2].offset = cursor;
  sections[2].size =
      (uint32_t)((size_t)cat.album_count * sizeof(AlbumRowV3));
  cursor += sections[2].size;

  sections[3].type = SEC_V3_TRACKS;
  sections[3].offset = cursor;
  sections[3].size =
      (uint32_t)((size_t)cat.track_count * sizeof(TrackRowV3));
  cursor += sections[3].size;

  if (cursor > kMaxIndexFileBytes) {
    LOGE("[曲库索引] 索引文件过大：%lu", (unsigned long)cursor);
    return false;
  }

  header.crc32 = calculate_catalog_crc(cat, sections);

  File32 file = sd.open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
  if (!file) {
    LOGE("[曲库索引] 打开临时文件失败：%s", tmp_path.c_str());
    return false;
  }

  bool write_ok = write_header(file, header);
  for (uint32_t i = 0; write_ok && i < kRequiredSectionCount; ++i) {
    write_ok = write_section(file, sections[i]);
  }

  write_ok = write_ok && write_blob(file, cat.pool.data, cat.pool.size);
  write_ok = write_ok && write_blob(
      file,
      cat.artists,
      (size_t)cat.artist_count * sizeof(ArtistRowV3));
  write_ok = write_ok && write_blob(
      file,
      cat.albums,
      (size_t)cat.album_count * sizeof(AlbumRowV3));
  write_ok = write_ok && write_blob(
      file,
      cat.tracks,
      (size_t)cat.track_count * sizeof(TrackRowV3));

  if (write_ok) {
    write_ok = file.sync();
  }
  file.close();

  if (!write_ok) {
    remove_if_exists_locked(tmp_path.c_str());
    LOGE("[曲库索引] 临时文件写入或同步失败：%s", tmp_path.c_str());
    return false;
  }

  File32 verify = sd.open(tmp_path.c_str(), O_RDONLY);
  IndexLayoutV3 tmp_layout;
  const LayoutValidationState tmp_verify_state = verify
      ? validate_file_layout(verify,
                             tmp_path.c_str(),
                             tmp_layout,
                             reason,
                             sizeof(reason))
      : LayoutValidationState::IoError;
  if (tmp_verify_state != LayoutValidationState::Valid) {
    if (verify) verify.close();
    remove_if_exists_locked(tmp_path.c_str());
    LOGE("[曲库索引] 临时文件落盘校验失败：%s", reason);
    return false;
  }
  verify.close();

  remove_if_exists_locked(backup_path.c_str());
  const bool had_final = path_exists_locked(final_path.c_str());
  if (had_final && !sd.rename(final_path.c_str(), backup_path.c_str())) {
    LOGE("[曲库索引] 无法创建旧索引备份，保留正式文件并放弃替换");
    return false;
  }

  if (!sd.rename(tmp_path.c_str(), final_path.c_str())) {
    LOGE("[曲库索引] 临时文件提升失败，尝试恢复旧索引");
    if (had_final && !sd.rename(backup_path.c_str(), final_path.c_str())) {
      LOGE("[曲库索引] 旧索引恢复失败；下次启动将从 .bak/.tmp 自动恢复");
    }
    return false;
  }

  File32 final_verify = sd.open(final_path.c_str(), O_RDONLY);
  IndexLayoutV3 final_layout;
  const LayoutValidationState final_verify_state = final_verify
      ? validate_file_layout(final_verify,
                             final_path.c_str(),
                             final_layout,
                             reason,
                             sizeof(reason))
      : LayoutValidationState::IoError;
  if (final_verify) final_verify.close();

  if (final_verify_state != LayoutValidationState::Valid) {
    LOGE("[曲库索引] 正式文件替换后校验失败：%s", reason);

    if (final_verify_state == LayoutValidationState::Corrupt) {
      quarantine_file_locked(final_path.c_str(), bad_path.c_str());
      if (had_final && !sd.rename(backup_path.c_str(), final_path.c_str())) {
        LOGE("[曲库索引] 校验失败后旧索引恢复失败");
      }
    } else {
      // 瞬时 I/O 错误时不判定文件损坏，同时保留 final 与 bak，
      // 下次启动会再次校验并选择可用副本。
      LOGW("[曲库索引] 正式文件校验遇到 I/O 错误，保留新旧副本等待下次恢复");
    }
    return false;
  }

  // 保留 .bak 作为上一份已知可用索引。单份通常只有数百 KB，
  // 可以在正式文件后续损坏时免去一次完整曲库重扫。
  LOGI("[曲库索引] 原子保存成功：%s size=%lu CRC=0x%08lx 歌曲=%lu 专辑=%lu 歌手=%lu 字符池=%lu 备份=%s",
       index_path,
       (unsigned long)final_layout.file_size,
       (unsigned long)header.crc32,
       (unsigned long)cat.track_count,
       (unsigned long)cat.album_count,
       (unsigned long)cat.artist_count,
       (unsigned long)cat.pool.size,
       had_final ? "已保留" : "首次保存无旧版");
  return true;
}

bool storage_index_load_v3(MusicCatalogV3& out_cat, const char* index_path)
{
  storage_catalog_v3_free(out_cat);
  s_last_load_needs_rewrite = false;
  s_last_load_source = "none";

  StorageSdLockGuard sd_lock(2000);
  if (!sd_lock) {
    LOGE("[曲库索引] 加载锁超时");
    return false;
  }

  const String final_path(index_path);
  const String tmp_path = final_path + ".tmp";
  const String backup_path = final_path + ".bak";
  const String bad_path = final_path + ".bad";
  const String bad_backup_path = final_path + ".bad.bak";

  char reason[192] = {};
  bool legacy_crc = false;

  /*
   * 正常运行结束后不会残留 .tmp。
   * 因此一个完整有效的 .tmp 表示上次原子替换在断电前已写完，应优先恢复它。
   */
  CandidateState state = load_candidate_locked(tmp_path.c_str(),
                                               out_cat,
                                               legacy_crc,
                                               reason,
                                               sizeof(reason));
  if (state == CandidateState::Valid) {
    const bool promoted = promote_candidate_locked(tmp_path.c_str(),
                                                   final_path.c_str(),
                                                   bad_path.c_str());
    s_last_load_needs_rewrite = legacy_crc || !promoted;
    s_last_load_source = "tmp-recovered";
    LOGW("[曲库索引] 从中断写入临时文件恢复成功：%s", tmp_path.c_str());
    return true;
  }
  if (state == CandidateState::Unavailable) {
    // 临时文件可能只是在断电后留下的半成品；读取失败时保留它，
    // 继续尝试正式索引，不让一个可疑 .tmp 阻断正常启动。
    LOGW("[曲库索引] 临时索引暂时不可用，继续检查正式文件：%s 原因=%s",
         tmp_path.c_str(),
         reason);
    storage_catalog_v3_free(out_cat);
  }
  if (state == CandidateState::Invalid) {
    LOGW("[曲库索引] 删除无效临时文件：%s 原因=%s",
         tmp_path.c_str(),
         reason);
    remove_if_exists_locked(tmp_path.c_str());
  }

  state = load_candidate_locked(final_path.c_str(),
                                out_cat,
                                legacy_crc,
                                reason,
                                sizeof(reason));
  if (state == CandidateState::Valid) {
    s_last_load_needs_rewrite = legacy_crc;
    s_last_load_source = "final";
    return true;
  }

  const bool final_unavailable = state == CandidateState::Unavailable;
  if (final_unavailable) {
    // I/O 或内存不足不等于文件损坏：不隔离、不删除，尝试只读备份兜底。
    LOGW("[曲库索引] 正式索引暂时不可用，尝试备份：%s 原因=%s",
         final_path.c_str(),
         reason);
    storage_catalog_v3_free(out_cat);
  }

  if (state == CandidateState::Invalid) {
    LOGE("[曲库索引] 正式索引损坏：%s 原因=%s",
         final_path.c_str(),
         reason);
    quarantine_file_locked(final_path.c_str(), bad_path.c_str());
  } else {
    LOGW("[曲库索引] 正式索引不存在：%s", final_path.c_str());
  }

  state = load_candidate_locked(backup_path.c_str(),
                                out_cat,
                                legacy_crc,
                                reason,
                                sizeof(reason));
  if (state == CandidateState::Valid) {
    if (final_unavailable) {
      // 正式文件尚未被证明损坏，只用备份启动本次内存曲库，
      // 不覆盖正式文件，也不触发自动重写，避免瞬时 I/O 导致版本回退。
      s_last_load_needs_rewrite = false;
      s_last_load_source = "bak-fallback";
      LOGW("[曲库索引] 正式文件暂时不可用，本次只读使用备份：%s",
           backup_path.c_str());
    } else {
      const bool promoted = promote_candidate_locked(backup_path.c_str(),
                                                     final_path.c_str(),
                                                     bad_path.c_str());
      if (!promoted) {
        LOGW("[曲库索引] 备份已加载但提升失败，将由后续原子重写再次修复");
      }
      // 备份被提升为正式文件后立即安排一次原子重写，
      // 让当前有效文件重新生成一份 .bak。
      s_last_load_needs_rewrite = true;
      s_last_load_source = "bak-recovered";
      LOGW("[曲库索引] 从备份恢复成功：%s", backup_path.c_str());
    }
    return true;
  }

  if (state == CandidateState::Unavailable) {
    LOGE("[曲库索引] 备份索引读取资源或 I/O 不可用：%s 原因=%s",
         backup_path.c_str(),
         reason);
    storage_catalog_v3_free(out_cat);
    return false;
  }

  if (state == CandidateState::Invalid) {
    LOGE("[曲库索引] 备份索引也损坏：%s 原因=%s",
         backup_path.c_str(),
         reason);
    quarantine_file_locked(backup_path.c_str(), bad_backup_path.c_str());
  }

  storage_catalog_v3_free(out_cat);
  LOGE("[曲库索引] 没有可用索引，将进入本地扫描重建");
  return false;
}

bool storage_index_last_load_needs_rewrite_v3(void)
{
  return s_last_load_needs_rewrite;
}

const char* storage_index_last_load_source_v3(void)
{
  return s_last_load_source;
}
