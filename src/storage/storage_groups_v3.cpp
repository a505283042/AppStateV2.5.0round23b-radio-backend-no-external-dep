#include "storage/storage_groups_v3.h"
#include <map>
#include <esp_heap_caps.h>
#include "utils/log.h"

static String pool_string_safe_v3(const StringPoolV3& pool, uint32_t off)
{
    const char* p = pool_str_v3(pool, off);
    return String(p ? p : "");
}

static std::map<String, uint32_t> build_artist_name_to_off_map_v3(const MusicCatalogV3& cat)
{
    std::map<String, uint32_t> out;
    if (!cat.artists || cat.artist_count == 0) return out;

    for (uint32_t i = 0; i < cat.artist_count; ++i) {
        const ArtistRowV3& ar = cat.artists[i];
        String name = pool_string_safe_v3(cat.pool, ar.name_off);
        if (!name.isEmpty()) {
            out[name] = ar.name_off;
        }
    }
    return out;
}

std::vector<String> storage_split_artists_v3(const String& artists_str)
{
    std::vector<String> result;

    if (artists_str.isEmpty()) {
        result.push_back("未知歌手");
        return result;
    }

    int start = 0;
    int end = artists_str.indexOf('/');

    while (end != -1) {
        String artist = artists_str.substring(start, end);
        artist.trim();
        if (artist.length() > 0) {
            result.push_back(artist);
        }
        start = end + 1;
        end = artists_str.indexOf('/', start);
    }

    String last = artists_str.substring(start);
    last.trim();
    if (last.length() > 0) {
        result.push_back(last);
    }

    if (result.empty()) {
        result.push_back("未知歌手");
    }

    return result;
}

static String first_artist_name_v3(const String& artists_str)
{
    if (artists_str.isEmpty()) {
        return String("未知歌手");
    }

    int start = 0;
    while (start <= artists_str.length()) {
        int end = artists_str.indexOf('/', start);
        String artist = (end == -1) ? artists_str.substring(start)
                                    : artists_str.substring(start, end);
        artist.trim();
        if (!artist.isEmpty()) {
            return artist;
        }
        if (end == -1) {
            break;
        }
        start = end + 1;
    }

    return String("未知歌手");
}

static TrackIndex16* alloc_group_track_pool_v3(uint32_t count, const char* label)
{
    if (count == 0) {
        return nullptr;
    }

    const size_t bytes = (size_t)count * sizeof(TrackIndex16);
    void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        LOGE("[曲库分组] %s 索引池 PSRAM 分配失败: 数量=%lu 字节=%lu",
             label ? label : "分组",
             (unsigned long)count,
             (unsigned long)bytes);
        return nullptr;
    }

    LOGD("[曲库分组] %s 索引池 分配=%lu 字节 (PSRAM)",
         label ? label : "分组",
         (unsigned long)bytes);
    return static_cast<TrackIndex16*>(p);
}

static uint32_t first_artist_off_for_track_v3(const MusicCatalogV3& cat,
                                              const std::map<String, uint32_t>& artist_name_to_off,
                                              uint32_t track_idx)
{
    const TrackRowV3& row = cat.tracks[track_idx];
    String artist_display = pool_string_safe_v3(cat.pool, row.artist_off);
    String first_artist = first_artist_name_v3(artist_display);

    auto ait = artist_name_to_off.find(first_artist);
    if (ait != artist_name_to_off.end()) {
        return ait->second;
    }

    return row.artist_off;
}

static bool increment_group_count_v3(PlaylistGroup& group, const char* label)
{
    if (group.track_indices.count == UINT16_MAX) {
        LOGE("[曲库分组] %s 分组歌曲数超过 TrackIndex16 上限", label ? label : "分组");
        return false;
    }

    ++group.track_indices.count;
    return true;
}

static bool build_artist_groups_v3(MusicCatalogV3& cat)
{
    cat.artist_groups.clear();

    std::map<uint32_t, int> artist_map;
    std::map<String, uint32_t> artist_name_to_off = build_artist_name_to_off_map_v3(cat);

    if (!cat.tracks || cat.track_count == 0) {
        LOGD("[曲库分组] no 歌曲s, 跳过 歌手 分组s");
        return true;
    }

    for (uint32_t i = 0; i < cat.track_count; ++i) {
        if (i > UINT16_MAX) {
            LOGE("[曲库分组] 无效 歌曲_索引=%lu", (unsigned long)i);
            continue;
        }

        const uint32_t first_artist_off = first_artist_off_for_track_v3(cat, artist_name_to_off, i);
        auto it = artist_map.find(first_artist_off);
        if (it == artist_map.end()) {
            PlaylistGroup g;
            g.name_off = first_artist_off;
            g.primary_artist_off = INVALID_OFF32;
            cat.artist_groups.push_back(g);

            int new_idx = (int)cat.artist_groups.size() - 1;
            artist_map[first_artist_off] = new_idx;
            it = artist_map.find(first_artist_off);
        }

        if (!increment_group_count_v3(cat.artist_groups[it->second], "歌手")) {
            return false;
        }
    }

    uint32_t total_indices = 0;
    for (const auto& group : cat.artist_groups) {
        total_indices += group.track_indices.count;
    }

    cat.artist_group_track_pool = alloc_group_track_pool_v3(total_indices, "歌手");
    if (total_indices > 0 && !cat.artist_group_track_pool) {
        return false;
    }
    cat.artist_group_track_pool_count = total_indices;

    uint32_t cursor = 0;
    for (auto& group : cat.artist_groups) {
        const uint16_t count = group.track_indices.count;
        group.track_indices.data = count ? (cat.artist_group_track_pool + cursor) : nullptr;
        group.track_indices.count = 0;
        cursor += count;
    }

    for (uint32_t i = 0; i < cat.track_count; ++i) {
        if (i > UINT16_MAX) {
            continue;
        }

        const uint32_t first_artist_off = first_artist_off_for_track_v3(cat, artist_name_to_off, i);
        auto it = artist_map.find(first_artist_off);
        if (it == artist_map.end()) {
            LOGE("[曲库分组] 歌手 第二遍找不到分组 off=%lu", (unsigned long)first_artist_off);
            return false;
        }

        PlaylistGroupTrackList& list = cat.artist_groups[it->second].track_indices;
        if (!list.data) {
            LOGE("[曲库分组] 歌手 第二遍空索引池 group=%d", it->second);
            return false;
        }
        list.data[list.count++] = (TrackIndex16)i;
    }

    LOGD("[曲库分组] 歌手 分组s=%d", (int)cat.artist_groups.size());
    return true;
}

static uint32_t album_group_key_for_track_v3(const MusicCatalogV3& cat, uint32_t track_idx)
{
    return cat.tracks[track_idx].album_id;
}

static void fill_album_group_meta_v3(const MusicCatalogV3& cat,
                                     uint32_t album_id,
                                     uint32_t& album_name_off,
                                     uint32_t& primary_artist_off)
{
    album_name_off = INVALID_OFF32;
    primary_artist_off = INVALID_OFF32;

    if (album_id != INVALID_ID32 && cat.albums && album_id < cat.album_count) {
        const AlbumRowV3& al = cat.albums[album_id];
        album_name_off = al.name_off;
        primary_artist_off = al.primary_artist_off;
        if (album_name_off == INVALID_OFF32) album_name_off = 0;
    }
}

static bool build_album_groups_v3(MusicCatalogV3& cat)
{
    cat.album_groups.clear();

    if (!cat.tracks || cat.track_count == 0) {
        LOGD("[曲库分组] no 歌曲s, 跳过 专辑 分组s");
        return true;
    }

    std::map<uint32_t, int> album_map;

    for (uint32_t i = 0; i < cat.track_count; ++i) {
        if (i > UINT16_MAX) {
            LOGE("[曲库分组] 无效 歌曲_索引=%lu", (unsigned long)i);
            continue;
        }

        const uint32_t album_id = album_group_key_for_track_v3(cat, i);
        auto it = album_map.find(album_id);
        if (it == album_map.end()) {
            uint32_t album_name_off = INVALID_OFF32;
            uint32_t primary_artist_off = INVALID_OFF32;
            fill_album_group_meta_v3(cat, album_id, album_name_off, primary_artist_off);

            PlaylistGroup g;
            g.name_off = album_name_off;
            g.primary_artist_off = primary_artist_off;
            cat.album_groups.push_back(g);

            int new_idx = (int)cat.album_groups.size() - 1;
            album_map[album_id] = new_idx;
            it = album_map.find(album_id);
        }

        if (!increment_group_count_v3(cat.album_groups[it->second], "专辑")) {
            return false;
        }
    }

    uint32_t total_indices = 0;
    for (const auto& group : cat.album_groups) {
        total_indices += group.track_indices.count;
    }

    cat.album_group_track_pool = alloc_group_track_pool_v3(total_indices, "专辑");
    if (total_indices > 0 && !cat.album_group_track_pool) {
        return false;
    }
    cat.album_group_track_pool_count = total_indices;

    uint32_t cursor = 0;
    for (auto& group : cat.album_groups) {
        const uint16_t count = group.track_indices.count;
        group.track_indices.data = count ? (cat.album_group_track_pool + cursor) : nullptr;
        group.track_indices.count = 0;
        cursor += count;
    }

    for (uint32_t i = 0; i < cat.track_count; ++i) {
        if (i > UINT16_MAX) {
            continue;
        }

        const uint32_t album_id = album_group_key_for_track_v3(cat, i);
        auto it = album_map.find(album_id);
        if (it == album_map.end()) {
            LOGE("[曲库分组] 专辑 第二遍找不到分组 album_id=%lu", (unsigned long)album_id);
            return false;
        }

        PlaylistGroupTrackList& list = cat.album_groups[it->second].track_indices;
        if (!list.data) {
            LOGE("[曲库分组] 专辑 第二遍空索引池 group=%d", it->second);
            return false;
        }
        list.data[list.count++] = (TrackIndex16)i;
    }

    LOGD("[曲库分组] 专辑 分组s=%d", (int)cat.album_groups.size());
    return true;
}

bool storage_build_groups_v3(MusicCatalogV3& cat)
{
    cat.clear_runtime_only();

    if (!cat.tracks || cat.track_count == 0) {
        LOGD("[曲库分组] 为空 目录");
        return true;
    }

    if (cat.track_count > UINT16_MAX) {
        LOGE("[曲库分组] 歌曲 数量 过大 for uint16_t indices: %lu",
             (unsigned long)cat.track_count);
        return false;
    }

    if (!build_artist_groups_v3(cat)) {
        LOGE("[曲库分组] 歌手 分组构建失败");
        cat.clear_runtime_only();
        return false;
    }

    if (!build_album_groups_v3(cat)) {
        LOGE("[曲库分组] 专辑 分组构建失败");
        cat.clear_runtime_only();
        return false;
    }

    LOGD("[曲库分组][内存] 歌手分组=%d 专辑分组=%d 单个歌曲索引大小=%u",
         (int)cat.artist_groups.size(),
         (int)cat.album_groups.size(),
         (unsigned)sizeof(uint16_t));
    return true;
}