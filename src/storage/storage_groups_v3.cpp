#include "storage/storage_groups_v3.h"
#include <map>
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

static void build_artist_groups_v3(MusicCatalogV3& cat)
{
    cat.artist_groups.clear();

    std::map<uint32_t, int> artist_map;
    std::map<String, uint32_t> artist_name_to_off = build_artist_name_to_off_map_v3(cat);

    if (!cat.tracks || cat.track_count == 0) {
        LOGI("[GROUPS_V3] no tracks, skip artist groups");
        return;
    }

    for (uint32_t i = 0; i < cat.track_count; ++i) {
        const TrackRowV3& row = cat.tracks[i];
        String artist_display = pool_string_safe_v3(cat.pool, row.artist_off);

        std::vector<String> artists = storage_split_artists_v3(artist_display);
        String first_artist = artists.empty() ? String("未知歌手") : artists[0];

        // 保护：检查曲目索引是否在有效范围内
        if (i > 65535) {
            LOGE("[GROUPS_V3] invalid track_idx=%d", i);
            continue;
        }

        uint32_t first_artist_off = INVALID_OFF32;
        auto ait = artist_name_to_off.find(first_artist);
        if (ait != artist_name_to_off.end()) {
            first_artist_off = ait->second;
        } else {
            // 理论上 builder 已经把 primary artist 放进 artist 表了
            // 真找不到时退回原 artist_display 的 offset，至少别丢组
            first_artist_off = row.artist_off;
        }

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

        cat.artist_groups[it->second].track_indices.push_back((TrackIndex16)i);
    }

    LOGI("[GROUPS_V3] artist groups=%d", (int)cat.artist_groups.size());
}

static void build_album_groups_v3(MusicCatalogV3& cat)
{
    cat.album_groups.clear();

    if (!cat.tracks || cat.track_count == 0) {
        LOGI("[GROUPS_V3] no tracks, skip album groups");
        return;
    }

    /* album_id 在 V3 里已经是去重后的专辑键，所以直接用 album_id 分组最稳 */
    std::map<uint32_t, int> album_map;

    for (uint32_t i = 0; i < cat.track_count; ++i) {
        const TrackRowV3& row = cat.tracks[i];

        // 保护：检查曲目索引是否在有效范围内
        if (i > 65535) {
            LOGE("[GROUPS_V3] invalid track_idx=%d", i);
            continue;
        }

        uint32_t album_name_off = INVALID_OFF32;
        uint32_t primary_artist_off = INVALID_OFF32;

        uint32_t album_id = row.album_id;
        if (album_id != INVALID_ID32 && cat.albums && album_id < cat.album_count) {
            const AlbumRowV3& al = cat.albums[album_id];
            album_name_off = al.name_off;
            primary_artist_off = al.primary_artist_off;
            if (album_name_off == INVALID_OFF32) album_name_off = 0;
        }

        auto it = album_map.find(album_id);
        if (it == album_map.end()) {
            PlaylistGroup g;
            g.name_off = album_name_off;
            g.primary_artist_off = primary_artist_off;
            cat.album_groups.push_back(g);

            int new_idx = (int)cat.album_groups.size() - 1;
            album_map[album_id] = new_idx;
            it = album_map.find(album_id);
        }

        cat.album_groups[it->second].track_indices.push_back((TrackIndex16)i);
    }

    LOGI("[GROUPS_V3] album groups=%d", (int)cat.album_groups.size());
}

void storage_build_groups_v3(MusicCatalogV3& cat)
{
    cat.artist_groups.clear();
    cat.album_groups.clear();

    if (!cat.tracks || cat.track_count == 0) {
        LOGI("[GROUPS_V3] empty catalog");
        return;
    }

    // 保护：检查曲目数是否超过 uint16_t 的最大值
    if (cat.track_count > 65535) {
        LOGE("[GROUPS_V3] track count too large for uint16_t indices: %d", cat.track_count);
        return;
    }

    build_artist_groups_v3(cat);
    build_album_groups_v3(cat);

    // 记录分组内存使用信息
    LOGI("[GROUPS][MEM] artist_groups=%d album_groups=%d sizeof(track_index)=%u",
         (int)cat.artist_groups.size(),
         (int)cat.album_groups.size(),
         (unsigned)sizeof(uint16_t));
}