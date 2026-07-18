#include "storage/storage_builder_v3.h"
#include "storage/storage_catalog_v3.h"
#include <map>
#include <vector>
#include <algorithm>
#include <string.h>

#include "esp_heap_caps.h"
#include "utils/log.h"

static constexpr size_t kInternalFallbackMaxBytes = 32 * 1024;

/* =========================
 * 内部 builder 临时结构
 * ========================= */

struct StringPoolBuilder {
  PsramVector<uint8_t> blob;
  PsramMap<PsramString, uint32_t> str_to_off;

  StringPoolBuilder() {
    clear();
  }

  void clear() {
    blob.clear();
    str_to_off.clear();

    // 保留 offset 0 给"空串/无效"
    blob.push_back(0);
  }

  uint32_t intern(const PsramString& s) {
    if (s.isEmpty()) return INVALID_OFF32;

    auto it = str_to_off.find(s);
    if (it != str_to_off.end()) {
      return it->second;
    }

    uint32_t off = (uint32_t)blob.size();

    const size_t n = s.length();
    blob.insert(blob.end(), (const uint8_t*)s.c_str(), (const uint8_t*)s.c_str() + n);
    blob.push_back(0);  // '\0'

    str_to_off.emplace(s, off);
    return off;
  }

  uint32_t intern(const String& s) {
    const PsramString psram_text(s);
    return intern(psram_text);
  }
};

struct AlbumKeyV3 {
  PsramString album_name;
  PsramString primary_artist;
  PsramString folder_cover;

  bool operator<(const AlbumKeyV3& other) const {
    if (album_name != other.album_name) return album_name < other.album_name;
    if (primary_artist != other.primary_artist) return primary_artist < other.primary_artist;
    return folder_cover < other.folder_cover;
  }
};

/* =========================
 * 工具函数
 * ========================= */

struct TextSliceV3 {
  const char* data = "";
  size_t length = 0;

  TextSliceV3() = default;
  TextSliceV3(const char* text, size_t text_length)
      : data(text ? text : ""), length(text_length) {}
};

static TextSliceV3 trim_ascii_space_v3(TextSliceV3 slice)
{
  while (slice.length > 0 &&
         (slice.data[0] == ' ' || slice.data[0] == '\t')) {
    ++slice.data;
    --slice.length;
  }

  while (slice.length > 0) {
    const char c = slice.data[slice.length - 1];
    if (c != ' ' && c != '\t') break;
    --slice.length;
  }
  return slice;
}

static TextSliceV3 text_slice_v3(const PsramString& text)
{
  TextSliceV3 slice{text.c_str(), text.length()};
  return trim_ascii_space_v3(slice);
}

static TextSliceV3 primary_artist_slice_v3(const PsramString& artist)
{
  TextSliceV3 slice{text_slice_v3(artist)};
  for (size_t i = 0; i < slice.length; ++i) {
    if (slice.data[i] == '/') {
      slice.length = i;
      break;
    }
  }
  return trim_ascii_space_v3(slice);
}

static unsigned char ascii_fold_v3(unsigned char value)
{
  if (value >= 'A' && value <= 'Z') {
    return (unsigned char)(value + ('a' - 'A'));
  }
  return value;
}

static int compare_text_slice_v3(TextSliceV3 left, TextSliceV3 right)
{
  const size_t common = left.length < right.length
      ? left.length
      : right.length;

  for (size_t i = 0; i < common; ++i) {
    const unsigned char a = ascii_fold_v3(
        (unsigned char)left.data[i]);
    const unsigned char b = ascii_fold_v3(
        (unsigned char)right.data[i]);
    if (a < b) return -1;
    if (a > b) return 1;
  }

  if (left.length < right.length) return -1;
  if (left.length > right.length) return 1;
  return 0;
}

static TextSliceV3 unknown_artist_slice_v3()
{
  static const char kUnknownArtist[] = "未知歌手";
  return TextSliceV3{kUnknownArtist, sizeof(kUnknownArtist) - 1};
}

static TextSliceV3 unknown_album_slice_v3()
{
  static const char kUnknownAlbum[] = "未知专辑";
  return TextSliceV3{kUnknownAlbum, sizeof(kUnknownAlbum) - 1};
}

static PsramString split_primary_artist(const PsramString& artist)
{
  TextSliceV3 slice = primary_artist_slice_v3(artist);
  if (slice.length == 0) {
    return PsramString("未知歌手");
  }

  PsramString result;
  if (!result.assign(slice.data, slice.length)) std::abort();
  return result;
}

static int extract_track_no_hint(const TrackBuildTempV3& track)
{
  const char* path = track.audio_rel.c_str();
  const size_t path_length = track.audio_rel.length();

  size_t name_start = 0;
  for (size_t i = 0; i < path_length; ++i) {
    if (path[i] == '/') name_start = i + 1;
  }

  size_t name_end = path_length;
  for (size_t i = path_length; i > name_start; --i) {
    if (path[i - 1] == '.') {
      name_end = i - 1;
      break;
    }
  }

  size_t cursor = name_start;
  while (cursor < name_end &&
         (path[cursor] == ' ' || path[cursor] == '\t')) {
    ++cursor;
  }

  int number = 0;
  bool has_digits = false;
  while (cursor < name_end &&
         path[cursor] >= '0' && path[cursor] <= '9') {
    has_digits = true;
    number = number * 10 + (path[cursor] - '0');
    ++cursor;
  }

  return has_digits ? number : 0x7fffffff;
}

static bool track_build_temp_less_v3(const TrackBuildTempV3& left,
                                     const TrackBuildTempV3& right)
{
  TextSliceV3 left_artist = primary_artist_slice_v3(left.artist);
  TextSliceV3 right_artist = primary_artist_slice_v3(right.artist);
  if (left_artist.length == 0) left_artist = unknown_artist_slice_v3();
  if (right_artist.length == 0) right_artist = unknown_artist_slice_v3();

  int compare = compare_text_slice_v3(left_artist, right_artist);
  if (compare != 0) return compare < 0;

  TextSliceV3 left_album = text_slice_v3(left.album);
  TextSliceV3 right_album = text_slice_v3(right.album);
  if (left.album.isEmpty()) left_album = unknown_album_slice_v3();
  if (right.album.isEmpty()) right_album = unknown_album_slice_v3();

  compare = compare_text_slice_v3(left_album, right_album);
  if (compare != 0) return compare < 0;

  const int left_track_no = extract_track_no_hint(left);
  const int right_track_no = extract_track_no_hint(right);
  if (left_track_no != right_track_no) {
    return left_track_no < right_track_no;
  }

  compare = compare_text_slice_v3(
      text_slice_v3(left.title),
      text_slice_v3(right.title));
  if (compare != 0) return compare < 0;

  return compare_text_slice_v3(
      text_slice_v3(left.audio_rel),
      text_slice_v3(right.audio_rel)) < 0;
}

static void* alloc_prefer_psram(size_t n)
{
  if (n == 0) return nullptr;

  void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p) return p;

  if (n > kInternalFallbackMaxBytes) {
    LOGW("[曲库构建] 大缓冲 PSRAM 分配失败，禁止回落内部RAM size=%lu",
         (unsigned long)n);
    return nullptr;
  }

  return heap_caps_malloc(n, MALLOC_CAP_8BIT);
}

static bool copy_blob_to_psram(const PsramVector<uint8_t>& src, uint8_t*& out_ptr, uint32_t& out_size)
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

bool storage_build_catalog_v3_from_temp(StorageTrackBuildListV3& tracks,
                                        MusicCatalogV3& out_cat)
{
  storage_catalog_v3_free(out_cat);

  if (tracks.empty()) {
    LOGE("[曲库构建] 临时输入歌曲为空");
    return false;
  }

  // 临时曲目表本身已经位于 PSRAM；直接原地排序，避免再复制一整份字符串对象。
  std::sort(tracks.begin(), tracks.end(), track_build_temp_less_v3);
  if (tracks.size() > UINT16_MAX) {
    LOGW("[曲库构建] 歌曲数量超过 TrackIndex16 上限: 总数=%lu 保留=%u 跳过=%lu",
         (unsigned long)tracks.size(),
         (unsigned)UINT16_MAX,
         (unsigned long)(tracks.size() - UINT16_MAX));
    tracks.resize(UINT16_MAX);
  }

  StringPoolBuilder pool_builder;

  /* album 去重表 */
  PsramMap<AlbumKeyV3, uint32_t> album_map;
  PsramVector<AlbumRowV3> album_rows;

  /* artist 去重表：先只存 primary artist */
  PsramMap<PsramString, uint32_t> artist_map;
  PsramVector<ArtistRowV3> artist_rows;

  /* track rows */
  PsramVector<TrackRowV3> track_rows;
  track_rows.reserve(tracks.size());

  for (const auto& t : tracks) {
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
    PsramString primary_artist = split_primary_artist(t.artist);
    auto ait = artist_map.find(primary_artist);
    if (ait == artist_map.end()) {
      ArtistRowV3 ar;
      ar.name_off = pool_builder.intern(primary_artist);

      uint32_t new_id = (uint32_t)artist_rows.size();
      artist_rows.push_back(ar);
      artist_map[primary_artist] = new_id;
    }

    /* 4) album 表：按 (album_name, primary_artist, folder_cover) 去重 */
    PsramString album_name = t.album.isEmpty()
        ? PsramString("未知专辑")
        : t.album;
    PsramString folder_cover = t.cover_path_rel;

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

  LOGI("[曲库构建][PSRAM] pool_blob=%luB ext=%d string_map_node_ext=%d album_rows=%luB ext=%d album_map_node_ext=%d artist_rows=%luB ext=%d artist_map_node_ext=%d track_rows=%luB ext=%d",
       (unsigned long)(pool_builder.blob.capacity() * sizeof(uint8_t)),
       (!pool_builder.blob.empty() && esp_ptr_external_ram(pool_builder.blob.data())) ? 1 : 0,
       (!pool_builder.str_to_off.empty() && esp_ptr_external_ram(&*pool_builder.str_to_off.begin())) ? 1 : 0,
       (unsigned long)(album_rows.capacity() * sizeof(AlbumRowV3)),
       (!album_rows.empty() && esp_ptr_external_ram(album_rows.data())) ? 1 : 0,
       (!album_map.empty() && esp_ptr_external_ram(&*album_map.begin())) ? 1 : 0,
       (unsigned long)(artist_rows.capacity() * sizeof(ArtistRowV3)),
       (!artist_rows.empty() && esp_ptr_external_ram(artist_rows.data())) ? 1 : 0,
       (!artist_map.empty() && esp_ptr_external_ram(&*artist_map.begin())) ? 1 : 0,
       (unsigned long)(track_rows.capacity() * sizeof(TrackRowV3)),
       (!track_rows.empty() && esp_ptr_external_ram(track_rows.data())) ? 1 : 0);

  /* 拷贝到最终 catalog */

  if (!copy_blob_to_psram(pool_builder.blob, out_cat.pool.data, out_cat.pool.size)) {
    LOGE("[曲库构建] 分配/copy string pool 失败");
    storage_catalog_v3_free(out_cat);
    return false;
  }

  if (!artist_rows.empty()) {
    size_t n = artist_rows.size() * sizeof(ArtistRowV3);
    out_cat.artists = (ArtistRowV3*)alloc_prefer_psram(n);
    if (!out_cat.artists) {
      LOGE("[曲库构建] 分配 歌手s 失败");
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
      LOGE("[曲库构建] 分配 专辑s 失败");
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
      LOGE("[曲库构建] 分配 歌曲s 失败");
      storage_catalog_v3_free(out_cat);
      return false;
    }
    memcpy(out_cat.tracks, track_rows.data(), n);
    out_cat.track_count = (uint32_t)track_rows.size();
  }

  out_cat.generation = 1;

  LOGI("[曲库构建] 从临时数据构建成功：歌曲=%lu 专辑=%lu 歌手=%lu 字符池=%lu",
       (unsigned long)out_cat.track_count,
       (unsigned long)out_cat.album_count,
       (unsigned long)out_cat.artist_count,
       (unsigned long)out_cat.pool.size);

  return true;
}