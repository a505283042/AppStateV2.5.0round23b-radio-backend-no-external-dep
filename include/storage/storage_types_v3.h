#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <vector>

/* ===== 基础类型定义 ===== */

/* 封面来源枚举 - 表示封面图片的来源 */
enum CoverSource : uint8_t {
  COVER_NONE = 0,
  COVER_MP3_APIC,
  COVER_FLAC_PICTURE,
  COVER_FILE_FALLBACK
};

/* 兼容层：运行时展开后的曲目信息（供 UI / Audio / NFC 继续复用） */
struct TrackInfo {
  String artist;       /* 艺术家名称 */
  String album;        /* 专辑名称 */
  String title;        /* 歌曲标题 */

  String audio_path;   /* 通用：mp3/flac 文件路径 */
  String ext;          /* ".mp3" ".flac" */
  String lrc_path;     /* 歌词文件路径，如 /Music/xxx.lrc，不存在则空 */

  /* 封面信息 */
  CoverSource cover_source = COVER_NONE;
  uint32_t cover_offset = 0;   /* 从文件开头算起 */
  uint32_t cover_size = 0;     /* 封面数据字节数 */
  String cover_mime;           /* "image/jpeg" / "image/png" */
  String cover_path;           /* fallback 文件路径 */
};


/* 播放列表分组 */
using TrackIndex16 = uint16_t;

struct PlaylistGroup {
  uint32_t name_off = 0;                  /* 歌手名或专辑名偏移 */
  uint32_t primary_artist_off = 0;        /* 仅专辑分组使用 */
  std::vector<TrackIndex16> track_indices;
};


/* ===== V3 专用类型定义 ===== */

/* ===== 基础常量 ===== */

static constexpr uint32_t INDEX_V3_MAGIC = 0x4D494458;   // 'MIDX'
static constexpr uint16_t INDEX_V3_VERSION = 3;
static constexpr uint32_t INVALID_OFF32 = 0;
static constexpr uint32_t INVALID_ID32  = 0xFFFFFFFFu;

/* ===== 扩展名编码 ===== */

enum TrackExtCode : uint8_t {
  EXT_UNKNOWN = 0,
  EXT_MP3     = 1,
  EXT_FLAC    = 2
};

/* ===== Track flags ===== */

enum TrackFlags : uint16_t {
  TF_NONE            = 0,
  TF_HAS_LRC         = 1 << 0,
  TF_HAS_EMBED_COVER = 1 << 1,
  TF_HAS_FILE_COVER  = 1 << 2,
  TF_IS_MP3          = 1 << 3,
  TF_IS_FLAC         = 1 << 4,
};

/* ===== StringPool ===== */

struct StringPoolV3 {
  uint8_t* data = nullptr;   // 建议放 PSRAM
  uint32_t size = 0;

  void clear() {
    data = nullptr;
    size = 0;
  }

  bool empty() const {
    return data == nullptr || size == 0;
  }
};

/* ===== 去重表 ===== */

struct ArtistRowV3 {
  uint32_t name_off = INVALID_OFF32;   // 指向 StringPool
};

struct AlbumRowV3 {
  uint32_t name_off = INVALID_OFF32;            // 专辑名
  uint32_t primary_artist_off = INVALID_OFF32;  // 主歌手显示名
  uint32_t folder_cover_off = INVALID_OFF32;    // 目录封面路径（相对路径或绝对路径，先随你）
};

/* ===== 轻量音轨行 ===== */

struct TrackRowV3 {
  uint32_t title_off = INVALID_OFF32;      // 标题
  uint32_t artist_off = INVALID_OFF32;     // 原始 artist 显示串，如 "郭静/张韶涵/范玮琪"
  uint32_t album_id = INVALID_ID32;        // 指向 AlbumRowV3
  uint32_t audio_rel_off = INVALID_OFF32;  // 相对路径，如 "xxx.flac"
  uint32_t lrc_rel_off = INVALID_OFF32;    // 相对路径，无则 0
  uint32_t cover_path_off = INVALID_OFF32; // fallback 封面路径，无则 0

  uint32_t cover_offset = 0;               // 内嵌封面偏移
  uint32_t cover_size = 0;                 // 内嵌封面大小

  uint32_t mime_off = INVALID_OFF32;       // image/jpeg 等。先用 uint32_t，省得以后不够

  uint8_t cover_source = COVER_NONE;
  uint8_t ext_code = EXT_UNKNOWN;

  uint16_t flags = TF_NONE;
  uint16_t reserved = 0;
};

/* ===== 索引文件段类型 ===== */

enum IndexSectionTypeV3 : uint32_t {
  SEC_V3_STR_POOL = 1,
  SEC_V3_ARTISTS  = 2,
  SEC_V3_ALBUMS   = 3,
  SEC_V3_TRACKS   = 4,
};

/* ===== 文件头与段表 ===== */

struct IndexV3Header {
  uint32_t magic = INDEX_V3_MAGIC;
  uint16_t version = INDEX_V3_VERSION;
  uint16_t flags = 0;

  uint32_t header_size = sizeof(IndexV3Header);
  uint32_t section_count = 0;

  uint32_t track_count = 0;
  uint32_t album_count = 0;
  uint32_t artist_count = 0;
  uint32_t string_pool_size = 0;

  uint32_t crc32 = 0;   // 先预留，第一版可写 0
};

struct IndexSectionV3 {
  uint32_t type = 0;
  uint32_t offset = 0;
  uint32_t size = 0;
};

/* ===== 运行时 Catalog ===== */

struct MusicCatalogV3 {
  StringPoolV3 pool;

  TrackRowV3* tracks = nullptr;
  uint32_t track_count = 0;

  AlbumRowV3* albums = nullptr;
  uint32_t album_count = 0;

  ArtistRowV3* artists = nullptr;
  uint32_t artist_count = 0;

  std::vector<PlaylistGroup> artist_groups;
  std::vector<PlaylistGroup> album_groups;

  uint32_t generation = 0;

  void clear_runtime_only() {
    artist_groups.clear();
    album_groups.clear();
    generation = 0;
  }

  void clear_all() {
    pool.clear();
    tracks = nullptr;
    track_count = 0;
    albums = nullptr;
    album_count = 0;
    artists = nullptr;
    artist_count = 0;
    clear_runtime_only();
  }

  bool empty() const {
    return tracks == nullptr || track_count == 0;
  }
};

/* ===== 便捷访问 ===== */

static inline const char* pool_str_v3(const StringPoolV3& pool, uint32_t off)
{
  if (off == INVALID_OFF32) return "";
  if (pool.data == nullptr) return "";
  if (off >= pool.size) return "";
  return (const char*)(pool.data + off);
}

/* ===== PlaylistGroup 辅助函数 ===== */

static inline const char* playlist_group_name_cstr(const MusicCatalogV3& cat,
                                                   const PlaylistGroup& g)
{
  return pool_str_v3(cat.pool, g.name_off);
}

static inline const char* playlist_group_primary_artist_cstr(const MusicCatalogV3& cat,
                                                             const PlaylistGroup& g)
{
  return pool_str_v3(cat.pool, g.primary_artist_off);
}

static inline String playlist_group_name_string(const MusicCatalogV3& cat,
                                                const PlaylistGroup& g)
{
  return String(playlist_group_name_cstr(cat, g));
}

static inline String playlist_group_primary_artist_string(const MusicCatalogV3& cat,
                                                          const PlaylistGroup& g)
{
  return String(playlist_group_primary_artist_cstr(cat, g));
}

static inline String playlist_group_display_string(const MusicCatalogV3& cat,
                                                   const PlaylistGroup& g)
{
  const char* name = playlist_group_name_cstr(cat, g);
  const char* pa = playlist_group_primary_artist_cstr(cat, g);
  if (pa && pa[0]) {
    return String(pa) + " - " + String(name ? name : "");
  }
  return String(name ? name : "");
}
