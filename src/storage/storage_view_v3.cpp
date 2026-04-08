#include "storage/storage_view_v3.h"
#include "utils/log.h"

/* =========================
 * 工具函数
 * ========================= */

static String join_music_root(const char* music_root, const char* rel)
{
  if (!rel || rel[0] == '\0') return String();

  String r(rel);
  if (r.startsWith("/")) {
    return r;   // 已经是绝对路径
  }

  String root = music_root ? String(music_root) : String("/Music");
  if (!root.endsWith("/")) root += "/";
  return root + r;
}

static String pool_string_safe(const StringPoolV3& pool, uint32_t off)
{
  const char* p = pool_str_v3(pool, off);
  return String(p ? p : "");
}

static String album_name_from_id(const MusicCatalogV3& cat, uint32_t album_id)
{
  if (album_id == INVALID_ID32) return String();
  if (!cat.albums) return String();
  if (album_id >= cat.album_count) return String();
  return pool_string_safe(cat.pool, cat.albums[album_id].name_off);
}

static uint8_t ext_code_from_audio_path(const String& path)
{
  String s = path;
  s.toLowerCase();
  if (s.endsWith(".mp3")) return EXT_MP3;
  if (s.endsWith(".flac")) return EXT_FLAC;
  return EXT_UNKNOWN;
}

/* =========================
 * 主适配函数
 * ========================= */

bool storage_make_track_view_v3(const MusicCatalogV3& cat,
                                uint32_t track_index,
                                TrackViewV3& out,
                                const char* music_root)
{
  out = TrackViewV3{};

  if (!cat.tracks || track_index >= cat.track_count) {
    return false;
  }

  const TrackRowV3& row = cat.tracks[track_index];

  out.valid = true;
  out.track_index = track_index;

  out.title = pool_string_safe(cat.pool, row.title_off);
  out.artist = pool_string_safe(cat.pool, row.artist_off);
  out.album = album_name_from_id(cat, row.album_id);

  String audio_rel = pool_string_safe(cat.pool, row.audio_rel_off);
  String lrc_rel = pool_string_safe(cat.pool, row.lrc_rel_off);
  String cover_rel = pool_string_safe(cat.pool, row.cover_path_off);

  out.audio_path = join_music_root(music_root, audio_rel.c_str());
  out.lrc_path = join_music_root(music_root, lrc_rel.c_str());
  out.cover_path = join_music_root(music_root, cover_rel.c_str());
  out.cover_mime = pool_string_safe(cat.pool, row.mime_off);

  out.cover_offset = row.cover_offset;
  out.cover_size = row.cover_size;
  out.cover_source = row.cover_source;
  out.ext_code = row.ext_code;
  out.flags = row.flags;
  out.album_id = row.album_id;

  /* 兜底：如果 ext_code 没填好，用路径补一次 */
  if (out.ext_code == EXT_UNKNOWN) {
    out.ext_code = ext_code_from_audio_path(out.audio_path);
  }

  return true;
}

bool storage_fill_trackinfo_from_v3(const MusicCatalogV3& cat,
                                    uint32_t track_index,
                                    TrackInfo& out,
                                    const char* music_root)
{
  TrackViewV3 v;
  if (!storage_make_track_view_v3(cat, track_index, v, music_root)) {
    out = TrackInfo{};
    return false;
  }

  out = TrackInfo{};
  out.title = v.title;
  out.artist = v.artist;
  out.album = v.album;

  out.audio_path = v.audio_path;
  out.lrc_path = v.lrc_path;
  out.cover_path = v.cover_path;
  out.cover_mime = v.cover_mime;

  out.cover_offset = v.cover_offset;
  out.cover_size = v.cover_size;
  out.cover_source = (CoverSource)v.cover_source;

  switch (v.ext_code) {
    case EXT_MP3:  out.ext = ".mp3"; break;
    case EXT_FLAC: out.ext = ".flac"; break;
    default:       out.ext = ""; break;
  }

  return true;
}