#include "storage/storage_builder_v3.h"
#include "storage/storage_catalog_v3.h"
#include <map>
#include <vector>
#include <algorithm>
#include <string.h>

#include "esp_heap_caps.h"
#include "utils/log.h"

/* =========================
 * 内部 builder 临时结构
 * ========================= */

struct StringPoolBuilder {
  std::vector<uint8_t> blob;
  std::map<String, uint32_t> str_to_off;

  StringPoolBuilder() {
    clear();
  }

  void clear() {
    blob.clear();
    str_to_off.clear();

    // 保留 offset 0 给"空串/无效"
    blob.push_back(0);
  }

  uint32_t intern(const String& s) {
    if (s.isEmpty()) return INVALID_OFF32;

    auto it = str_to_off.find(s);
    if (it != str_to_off.end()) {
      return it->second;
    }

    uint32_t off = (uint32_t)blob.size();

    const size_t n = s.length();
    blob.insert(blob.end(), (const uint8_t*)s.c_str(), (const uint8_t*)s.c_str() + n);
    blob.push_back(0);  // '\0'

    str_to_off[s] = off;
    return off;
  }
};

struct AlbumKeyV3 {
  String album_name;
  String primary_artist;
  String folder_cover;

  bool operator<(const AlbumKeyV3& other) const {
    if (album_name != other.album_name) return album_name < other.album_name;
    if (primary_artist != other.primary_artist) return primary_artist < other.primary_artist;
    return folder_cover < other.folder_cover;
  }
};

/* =========================
 * 工具函数
 * ========================= */

static String split_primary_artist(const String& artist)
{
  if (artist.isEmpty()) return "未知歌手";

  int p = artist.indexOf('/');
  if (p < 0) {
    String s = artist;
    s.trim();
    return s.isEmpty() ? String("未知歌手") : s;
  }

  String s = artist.substring(0, p);
  s.trim();
  return s.isEmpty() ? String("未知歌手") : s;
}

static String normalized_sort_key(String s)
{
  s.trim();
  s.toLowerCase();
  return s;
}

static String basename_no_ext_from_rel_path(const String& rel_path)
{
  int slash = rel_path.lastIndexOf('/');
  String name = (slash >= 0) ? rel_path.substring(slash + 1) : rel_path;
  int dot = name.lastIndexOf('.');
  if (dot > 0) {
    name = name.substring(0, dot);
  }
  name.trim();
  return name;
}

static int extract_track_no_hint(const TrackBuildTempV3& t)
{
  String s = basename_no_ext_from_rel_path(t.audio_rel);
  int i = 0;
  while (i < s.length() && (s[i] == ' ' || s[i] == '	')) ++i;

  int num = 0;
  bool has_digits = false;
  while (i < s.length() && s[i] >= '0' && s[i] <= '9') {
    has_digits = true;
    num = num * 10 + (s[i] - '0');
    ++i;
  }

  if (!has_digits) return 0x7fffffff;

  while (i < s.length() && (s[i] == ' ' || s[i] == '	' || s[i] == '.' || s[i] == '-' || s[i] == '_')) {
    ++i;
  }

  return num;
}

static bool track_build_temp_less_v3(const TrackBuildTempV3& a, const TrackBuildTempV3& b)
{
  String a_artist = normalized_sort_key(split_primary_artist(a.artist));
  String b_artist = normalized_sort_key(split_primary_artist(b.artist));
  if (a_artist != b_artist) return a_artist < b_artist;

  String a_album = normalized_sort_key(a.album.isEmpty() ? String("未知专辑") : a.album);
  String b_album = normalized_sort_key(b.album.isEmpty() ? String("未知专辑") : b.album);
  if (a_album != b_album) return a_album < b_album;

  int a_track_no = extract_track_no_hint(a);
  int b_track_no = extract_track_no_hint(b);
  if (a_track_no != b_track_no) return a_track_no < b_track_no;

  String a_title = normalized_sort_key(a.title);
  String b_title = normalized_sort_key(b.title);
  if (a_title != b_title) return a_title < b_title;

  String a_path = normalized_sort_key(a.audio_rel);
  String b_path = normalized_sort_key(b.audio_rel);
  return a_path < b_path;
}

static void* alloc_prefer_psram(size_t n)
{
  if (n == 0) return nullptr;

  void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p) return p;

  return heap_caps_malloc(n, MALLOC_CAP_8BIT);
}

static bool copy_blob_to_psram(const std::vector<uint8_t>& src, uint8_t*& out_ptr, uint32_t& out_size)
{
  out_ptr = nullptr;
  out_size = 0;

  if (src.empty()) return true;

  void* mem = alloc_prefer_psram(src.size());
  if (!mem) return false;

  memcpy(mem, src.data(), src.size());
  out_ptr = (uint8_t*)mem;
  out_size = (uint32_t)src.size();
  return true;
}

/* =========================
 * 主构建流程
 * ========================= */

bool storage_build_catalog_v3_from_temp(const std::vector<TrackBuildTempV3>& tracks,
                                        MusicCatalogV3& out_cat)
{
  storage_catalog_v3_free(out_cat);

  if (tracks.empty()) {
    LOGE("[BUILDER_V3] temp input tracks empty");
    return false;
  }

  std::vector<TrackBuildTempV3> sorted_tracks = tracks;
  std::stable_sort(sorted_tracks.begin(), sorted_tracks.end(), track_build_temp_less_v3);

  StringPoolBuilder pool_builder;

  /* album 去重表 */
  std::map<AlbumKeyV3, uint32_t> album_map;
  std::vector<AlbumRowV3> album_rows;

  /* artist 去重表：先只存 primary artist */
  std::map<String, uint32_t> artist_map;
  std::vector<ArtistRowV3> artist_rows;

  /* track rows */
  std::vector<TrackRowV3> track_rows;
  track_rows.reserve(tracks.size());

  for (const auto& t : sorted_tracks) {
    TrackRowV3 row{};

    /* 1) 基本文本 */
    row.title_off = pool_builder.intern(t.title);
    row.artist_off = pool_builder.intern(t.artist);
    row.audio_rel_off = pool_builder.intern(t.audio_rel);
    row.lrc_rel_off = pool_builder.intern(t.lrc_rel);
    row.cover_path_off = pool_builder.intern(t.cover_path_rel);
    row.mime_off = pool_builder.intern(t.cover_mime);

    /* 2) 封面 / 扩展 / flags */
    row.cover_source = (uint8_t)t.cover_source;
    row.cover_offset = t.cover_offset;
    row.cover_size = t.cover_size;
    row.ext_code = t.ext_code;
    row.flags = t.flags;

    /* 3) artist 表：先仅保存 primary artist */
    String primary_artist = split_primary_artist(t.artist);
    auto ait = artist_map.find(primary_artist);
    if (ait == artist_map.end()) {
      ArtistRowV3 ar;
      ar.name_off = pool_builder.intern(primary_artist);

      uint32_t new_id = (uint32_t)artist_rows.size();
      artist_rows.push_back(ar);
      artist_map[primary_artist] = new_id;
    }

    /* 4) album 表：按 (album_name, primary_artist, folder_cover) 去重 */
    String album_name = t.album.isEmpty() ? String("未知专辑") : t.album;
    String folder_cover = t.cover_path_rel;

    AlbumKeyV3 ak;
    ak.album_name = album_name;
    ak.primary_artist = primary_artist;
    ak.folder_cover = folder_cover;

    auto alit = album_map.find(ak);
    if (alit == album_map.end()) {
      AlbumRowV3 al;
      al.name_off = pool_builder.intern(album_name);
      al.primary_artist_off = pool_builder.intern(primary_artist);
      al.folder_cover_off = pool_builder.intern(folder_cover);

      uint32_t new_album_id = (uint32_t)album_rows.size();
      album_rows.push_back(al);
      album_map[ak] = new_album_id;
      row.album_id = new_album_id;
    } else {
      row.album_id = alit->second;
    }

    track_rows.push_back(row);
  }

  /* 拷贝到最终 catalog */

  if (!copy_blob_to_psram(pool_builder.blob, out_cat.pool.data, out_cat.pool.size)) {
    LOGE("[BUILDER_V3] alloc/copy string pool failed");
    storage_catalog_v3_free(out_cat);
    return false;
  }

  if (!artist_rows.empty()) {
    size_t n = artist_rows.size() * sizeof(ArtistRowV3);
    out_cat.artists = (ArtistRowV3*)alloc_prefer_psram(n);
    if (!out_cat.artists) {
      LOGE("[BUILDER_V3] alloc artists failed");
      storage_catalog_v3_free(out_cat);
      return false;
    }
    memcpy(out_cat.artists, artist_rows.data(), n);
    out_cat.artist_count = (uint32_t)artist_rows.size();
  }

  if (!album_rows.empty()) {
    size_t n = album_rows.size() * sizeof(AlbumRowV3);
    out_cat.albums = (AlbumRowV3*)alloc_prefer_psram(n);
    if (!out_cat.albums) {
      LOGE("[BUILDER_V3] alloc albums failed");
      storage_catalog_v3_free(out_cat);
      return false;
    }
    memcpy(out_cat.albums, album_rows.data(), n);
    out_cat.album_count = (uint32_t)album_rows.size();
  }

  if (!track_rows.empty()) {
    size_t n = track_rows.size() * sizeof(TrackRowV3);
    out_cat.tracks = (TrackRowV3*)alloc_prefer_psram(n);
    if (!out_cat.tracks) {
      LOGE("[BUILDER_V3] alloc tracks failed");
      storage_catalog_v3_free(out_cat);
      return false;
    }
    memcpy(out_cat.tracks, track_rows.data(), n);
    out_cat.track_count = (uint32_t)track_rows.size();
  }

  out_cat.generation = 1;

  LOGI("[BUILDER_V3] build from temp ok: tracks=%lu albums=%lu artists=%lu pool=%lu",
       (unsigned long)out_cat.track_count,
       (unsigned long)out_cat.album_count,
       (unsigned long)out_cat.artist_count,
       (unsigned long)out_cat.pool.size);

  return true;
}