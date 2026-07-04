#include "player_playlist.h"

#include <algorithm>
#include <random>

#include <Arduino.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include "app_flags.h"
#include "storage/storage_catalog_v3.h"
#include "storage/storage_groups_v3.h"
#include "utils/log.h"
#include "app_diagnostics.h"

namespace {

int s_last_playlist_track_total = -1;
bool s_last_playlist_use_v3 = false;
int s_current_group_idx = 0;
uint16_t* s_current_playlist = nullptr;
size_t s_current_playlist_count = 0;
size_t s_current_playlist_cap = 0;
int s_current_playlist_pos = -1;
int s_original_group_pos = -1;
play_mode_t s_last_play_mode = PLAY_MODE_ALL_SEQ;
int s_last_group_idx = -1;

static void log_ptr_region_playlist(const char* label, const void* ptr, size_t bytes)
{
#if APP_DIAG_RAM_ATTRIBUTION
    LOGI("[内存归因] %s ptr=%p bytes=%lu internal=%d psram=%d",
         label,
         ptr,
         (unsigned long)bytes,
         ptr ? (esp_ptr_internal(ptr) ? 1 : 0) : 0,
         ptr ? (esp_ptr_external_ram(ptr) ? 1 : 0) : 0);
#else
    (void)label;
    (void)ptr;
    (void)bytes;
#endif
}


static void free_current_playlist()
{
    if (s_current_playlist) {
        heap_caps_free(s_current_playlist);
        s_current_playlist = nullptr;
    }
    s_current_playlist_count = 0;
    s_current_playlist_cap = 0;
}

static void clear_current_playlist()
{
    s_current_playlist_count = 0;
}

static bool reserve_current_playlist(size_t required_count)
{
    if (required_count <= s_current_playlist_cap) {
        return true;
    }

    if (required_count > UINT16_MAX) {
        LOGE("[播放器] 当前播放列表 超过 TrackIndex16 上限: 数量=%lu", (unsigned long)required_count);
        return false;
    }

    size_t new_cap = s_current_playlist_cap ? s_current_playlist_cap : 256;
    while (new_cap < required_count) {
        new_cap *= 2;
    }
    if (new_cap > UINT16_MAX) {
        new_cap = UINT16_MAX;
    }

    const size_t bytes = new_cap * sizeof(uint16_t);
    void* p = heap_caps_realloc(s_current_playlist, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        LOGE("[播放器] 当前播放列表 PSRAM 分配失败: 数量=%lu 字节=%lu",
             (unsigned long)new_cap,
             (unsigned long)bytes);
        return false;
    }

    s_current_playlist = static_cast<uint16_t*>(p);
    s_current_playlist_cap = new_cap;
    return true;
}

static bool build_all_current_playlist(int total)
{
    clear_current_playlist();
    if (total <= 0) {
        return true;
    }
    if (total > (int)UINT16_MAX) {
        LOGE("[播放器] 当前播放列表 总歌曲数超过上限: 数量=%d", total);
        return false;
    }
    if (!reserve_current_playlist((size_t)total)) {
        return false;
    }
    for (int i = 0; i < total; ++i) {
        s_current_playlist[s_current_playlist_count++] = (uint16_t)i;
    }
    return true;
}

static bool assign_current_playlist_from_indices(const PlaylistGroupTrackList& indices)
{
    clear_current_playlist();
    if (indices.empty()) {
        return true;
    }
    if (!reserve_current_playlist(indices.size())) {
        return false;
    }
    for (size_t i = 0; i < indices.size(); ++i) {
        s_current_playlist[s_current_playlist_count++] = (uint16_t)indices[i];
    }
    return true;
}

static bool current_playlist_empty()
{
    return s_current_playlist_count == 0;
}

static int current_playlist_track_at(size_t pos)
{
    if (pos >= s_current_playlist_count || !s_current_playlist) {
        return -1;
    }
    return (int)s_current_playlist[pos];
}

std::mt19937 g_rng(0x13572468u);
bool s_rng_seeded = false;

using CompactIndex = int16_t;
static constexpr CompactIndex kInvalidCompactIndex = (CompactIndex)-1;

CompactIndex* s_artist_group_index_by_track = nullptr;
CompactIndex* s_artist_group_pos_by_track = nullptr;
CompactIndex* s_album_group_index_by_track = nullptr;
CompactIndex* s_album_group_pos_by_track = nullptr;
size_t s_group_cache_size = 0;
uint32_t s_group_cache_generation = 0;

CompactIndex* s_playlist_pos_by_track = nullptr;
size_t s_playlist_pos_size = 0;
uint32_t s_playlist_pos_generation = 0;
play_mode_t s_playlist_pos_mode = PLAY_MODE_ALL_SEQ;
int s_playlist_pos_group_idx = -1;

int player_track_count_for_dispatch()
{
    return (int)storage_catalog_v3_track_count();
}

const std::vector<PlaylistGroup>& player_artist_groups_internal()
{
    return storage_catalog_v3_artist_groups();
}

const std::vector<PlaylistGroup>& player_album_groups_internal()
{
    return storage_catalog_v3_album_groups();
}

static bool ensure_compact_index_buf(CompactIndex*& buf,
                                     size_t& cur_size,
                                     size_t want_size,
                                     const char* tag)
{
    if (want_size == 0) {
        if (buf) {
            heap_caps_free(buf);
            buf = nullptr;
        }
        cur_size = 0;
        return true;
    }

    if (buf && cur_size == want_size) {
        return true;
    }

    void* p = heap_caps_realloc(buf,
                                want_size * sizeof(CompactIndex),
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        LOGE("[播放器] 分配 %s 失败: 数量=%u 字节=%u",
             tag,
             (unsigned)want_size,
             (unsigned)(want_size * sizeof(CompactIndex)));
        return false;
    }

    buf = (CompactIndex*)p;
    cur_size = want_size;
    return true;
}

static void fill_compact_index_buf(CompactIndex* buf,
                                   size_t n,
                                   CompactIndex value = kInvalidCompactIndex)
{
    if (!buf || n == 0) return;
    for (size_t i = 0; i < n; ++i) {
        buf[i] = value;
    }
}

static void free_group_cache_buffers()
{
    if (s_artist_group_index_by_track) {
        heap_caps_free(s_artist_group_index_by_track);
        s_artist_group_index_by_track = nullptr;
    }
    if (s_artist_group_pos_by_track) {
        heap_caps_free(s_artist_group_pos_by_track);
        s_artist_group_pos_by_track = nullptr;
    }
    if (s_album_group_index_by_track) {
        heap_caps_free(s_album_group_index_by_track);
        s_album_group_index_by_track = nullptr;
    }
    if (s_album_group_pos_by_track) {
        heap_caps_free(s_album_group_pos_by_track);
        s_album_group_pos_by_track = nullptr;
    }
    s_group_cache_size = 0;
}

static void free_playlist_pos_buffer()
{
    if (s_playlist_pos_by_track) {
        heap_caps_free(s_playlist_pos_by_track);
        s_playlist_pos_by_track = nullptr;
    }
    s_playlist_pos_size = 0;
}

void rebuild_group_cache_if_needed()
{
    const MusicCatalogV3& cat = storage_catalog_v3();
    if (!storage_catalog_v3_ready()) {
        free_group_cache_buffers();
        s_group_cache_generation = 0;
        return;
    }

    if (s_group_cache_generation == cat.generation &&
        s_group_cache_size == (size_t)cat.track_count &&
        s_artist_group_index_by_track &&
        s_album_group_index_by_track) {
        return;
    }

    const size_t n = (size_t)cat.track_count;
    if (!ensure_compact_index_buf(s_artist_group_index_by_track, s_group_cache_size, n, "artist_group_index") ||
        !ensure_compact_index_buf(s_artist_group_pos_by_track,   s_group_cache_size, n, "artist_group_pos") ||
        !ensure_compact_index_buf(s_album_group_index_by_track,  s_group_cache_size, n, "album_group_index") ||
        !ensure_compact_index_buf(s_album_group_pos_by_track,    s_group_cache_size, n, "album_group_pos")) {
        free_group_cache_buffers();
        s_group_cache_generation = 0;
        return;
    }

    fill_compact_index_buf(s_artist_group_index_by_track, n);
    fill_compact_index_buf(s_artist_group_pos_by_track, n);
    fill_compact_index_buf(s_album_group_index_by_track, n);
    fill_compact_index_buf(s_album_group_pos_by_track, n);

    const auto& artist_groups = player_artist_groups_internal();
    for (int gi = 0; gi < (int)artist_groups.size(); ++gi) {
        const auto& indices = artist_groups[gi].track_indices;
        for (int pos = 0; pos < (int)indices.size(); ++pos) {
            int track_idx = (int)indices[pos];
            if (track_idx >= 0 && track_idx < (int)n) {
                s_artist_group_index_by_track[(size_t)track_idx] = (CompactIndex)gi;
                s_artist_group_pos_by_track[(size_t)track_idx] = (CompactIndex)pos;
            }
        }
    }

    const auto& album_groups = player_album_groups_internal();
    for (int gi = 0; gi < (int)album_groups.size(); ++gi) {
        const auto& indices = album_groups[gi].track_indices;
        for (int pos = 0; pos < (int)indices.size(); ++pos) {
            int track_idx = (int)indices[pos];
            if (track_idx >= 0 && track_idx < (int)n) {
                s_album_group_index_by_track[(size_t)track_idx] = (CompactIndex)gi;
                s_album_group_pos_by_track[(size_t)track_idx] = (CompactIndex)pos;
            }
        }
    }

    s_group_cache_generation = cat.generation;
    LOGD("[播放器] 分组 缓存 已重建: 歌曲s=%lu 歌手s=%d 专辑s=%d gen=%lu",
         (unsigned long)cat.track_count,
         (int)artist_groups.size(),
         (int)album_groups.size(),
         (unsigned long)s_group_cache_generation);
    
    LOGD("[播放器][缓存] 分组缓存：歌曲=%u 字节=%u（PSRAM）",
         (unsigned)n,
         (unsigned)(n * 4 * sizeof(CompactIndex)));

    log_ptr_region_playlist("playlist.artist_group_index", s_artist_group_index_by_track, n * sizeof(CompactIndex));
    log_ptr_region_playlist("playlist.artist_group_pos", s_artist_group_pos_by_track, n * sizeof(CompactIndex));
    log_ptr_region_playlist("playlist.album_group_index", s_album_group_index_by_track, n * sizeof(CompactIndex));
    log_ptr_region_playlist("playlist.album_group_pos", s_album_group_pos_by_track, n * sizeof(CompactIndex));
}

void rebuild_playlist_pos_cache()
{
    const MusicCatalogV3& cat = storage_catalog_v3();
    const size_t n = storage_catalog_v3_ready() ? (size_t)cat.track_count : 0u;
    if (!ensure_compact_index_buf(s_playlist_pos_by_track, s_playlist_pos_size, n, "playlist_pos")) {
        free_playlist_pos_buffer();
        s_playlist_pos_generation = 0;
        return;
    }
    fill_compact_index_buf(s_playlist_pos_by_track, n);

    for (int pos = 0; pos < (int)s_current_playlist_count; ++pos) {
        int track_idx = s_current_playlist[(size_t)pos];
        if (track_idx >= 0 && track_idx < (int)n) {
            s_playlist_pos_by_track[(size_t)track_idx] = (CompactIndex)pos;
        }
    }

    s_playlist_pos_generation = storage_catalog_v3_ready() ? cat.generation : 0;
    s_playlist_pos_mode = g_play_mode;
    s_playlist_pos_group_idx = s_current_group_idx;
    
    LOGD("[播放器][缓存] 播放列表位置：歌曲=%u 字节=%u（PSRAM）",
         (unsigned)n,
         (unsigned)(n * sizeof(CompactIndex)));

    log_ptr_region_playlist("playlist.pos_by_track", s_playlist_pos_by_track, n * sizeof(CompactIndex));
}

void shuffle_playlist_keep_current_front(uint16_t* playlist, size_t count, int current_track)
{
    if (!playlist || count == 0) return;

    player_playlist_seed_rng_once();

    size_t current_pos = count;
    for (size_t i = 0; i < count; ++i) {
        if (playlist[i] == (uint16_t)current_track) {
            current_pos = i;
            break;
        }
    }

    if (current_pos >= count) {
        std::shuffle(playlist, playlist + count, g_rng);
        return;
    }

    std::swap(playlist[0], playlist[current_pos]);
    if (count > 1) {
        std::shuffle(playlist + 1, playlist + count, g_rng);
    }
}

int find_pos_in_playlist(int track_idx)
{
    if (track_idx >= 0 && track_idx < (int)s_playlist_pos_size && s_playlist_pos_by_track) {
        return (int)s_playlist_pos_by_track[(size_t)track_idx];
    }
    return -1;
}

void update_playlist_cache_for_track(int current_track_idx)
{
    bool need_update = false;

    const int total = player_track_count_for_dispatch();
    const bool using_v3 = storage_catalog_v3_ready();

    if (g_play_mode != s_last_play_mode) {
        need_update = true;
    } else if ((player_playlist_is_artist_mode(g_play_mode) ||
                player_playlist_is_album_mode(g_play_mode)) &&
               s_current_group_idx != s_last_group_idx) {
        need_update = true;
    } else if (total != s_last_playlist_track_total) {
        need_update = true;
    } else if (using_v3 != s_last_playlist_use_v3) {
        need_update = true;
    } else if (using_v3 && s_playlist_pos_generation != storage_catalog_v3().generation) {
        need_update = true;
    }

    if (need_update) {
        clear_current_playlist();
        s_current_playlist_pos = -1;
        bool playlist_build_ok = true;

        switch (g_play_mode) {
            case PLAY_MODE_ALL_SEQ:
            case PLAY_MODE_ALL_RND:
                playlist_build_ok = build_all_current_playlist(total);
                break;

            case PLAY_MODE_ARTIST_SEQ:
            case PLAY_MODE_ARTIST_RND: {
                const auto& groups = player_artist_groups_internal();
                if (s_current_group_idx < 0 || s_current_group_idx >= (int)groups.size()) {
                    s_current_group_idx = 0;
                }
                if (!groups.empty()) {
                    playlist_build_ok = assign_current_playlist_from_indices(groups[s_current_group_idx].track_indices);
                }
                break;
            }

            case PLAY_MODE_ALBUM_SEQ:
            case PLAY_MODE_ALBUM_RND: {
                const auto& groups = player_album_groups_internal();
                if (s_current_group_idx < 0 || s_current_group_idx >= (int)groups.size()) {
                    s_current_group_idx = 0;
                }
                if (!groups.empty()) {
                    playlist_build_ok = assign_current_playlist_from_indices(groups[s_current_group_idx].track_indices);
                }
                break;
            }
        }

        if (!playlist_build_ok) {
            clear_current_playlist();
        }

        s_original_group_pos = -1;
        if (player_playlist_is_artist_mode(g_play_mode)) {
            rebuild_group_cache_if_needed();
            if (current_track_idx >= 0 && current_track_idx < (int)s_group_cache_size && s_artist_group_pos_by_track) {
                s_original_group_pos = (int)s_artist_group_pos_by_track[(size_t)current_track_idx];
            }
        } else if (player_playlist_is_album_mode(g_play_mode)) {
            rebuild_group_cache_if_needed();
            if (current_track_idx >= 0 && current_track_idx < (int)s_group_cache_size && s_album_group_pos_by_track) {
                s_original_group_pos = (int)s_album_group_pos_by_track[(size_t)current_track_idx];
            }
        } else if (!current_playlist_empty() && current_track_idx >= 0 && current_track_idx < total) {
            s_original_group_pos = current_track_idx;
        }

        const bool is_rnd = (g_play_mode == PLAY_MODE_ALL_RND ||
                             g_play_mode == PLAY_MODE_ARTIST_RND ||
                             g_play_mode == PLAY_MODE_ALBUM_RND);
        if (is_rnd && !current_playlist_empty()) {
            shuffle_playlist_keep_current_front(s_current_playlist, s_current_playlist_count, current_track_idx);
        }

        rebuild_playlist_pos_cache();
        s_current_playlist_pos = find_pos_in_playlist(current_track_idx);

        if ((player_playlist_is_artist_mode(g_play_mode) || player_playlist_is_album_mode(g_play_mode)) &&
            !current_playlist_empty() && s_current_playlist_pos < 0) {
            s_current_playlist_pos = 0;
        }

        log_ptr_region_playlist("playlist.current",
                                s_current_playlist,
                                s_current_playlist_cap * sizeof(uint16_t));

        s_last_play_mode = g_play_mode;
        s_last_group_idx = s_current_group_idx;
        s_last_playlist_track_total = total;
        s_last_playlist_use_v3 = using_v3;
    } else {
        const uint32_t current_gen = using_v3 ? storage_catalog_v3().generation : 0;
        if (s_playlist_pos_size != (size_t)total ||
            s_playlist_pos_generation != current_gen ||
            s_playlist_pos_mode != g_play_mode ||
            s_playlist_pos_group_idx != s_current_group_idx) {
            rebuild_playlist_pos_cache();
        }
    }
}

} // namespace

void player_playlist_seed_rng_once()
{
    if (s_rng_seeded) return;

    uint32_t seed = esp_random() ^ (uint32_t)micros() ^ ((uint32_t)ESP.getFreeHeap() << 1);
    if (seed == 0) seed = 0xA5A55A5Au;
    g_rng.seed(seed);
    s_rng_seeded = true;
    LOGD("[播放器] rng seeded: 0x%08lx", (unsigned long)seed);
}

void player_playlist_reset_state()
{
    s_last_playlist_track_total = -1;
    s_last_playlist_use_v3 = false;
    s_current_group_idx = 0;
    free_current_playlist();
    s_current_playlist_pos = -1;
    s_original_group_pos = -1;
    s_last_play_mode = PLAY_MODE_ALL_SEQ;
    s_last_group_idx = -1;

    free_group_cache_buffers();
    s_group_cache_generation = 0;

    free_playlist_pos_buffer();
    s_playlist_pos_generation = 0;
    s_playlist_pos_mode = PLAY_MODE_ALL_SEQ;
    s_playlist_pos_group_idx = -1;
}

void player_playlist_force_rebuild()
{
    s_last_play_mode = (play_mode_t)-1;
    s_last_group_idx = -1;
}

bool player_playlist_is_artist_mode(play_mode_t mode)
{
    return mode == PLAY_MODE_ARTIST_SEQ || mode == PLAY_MODE_ARTIST_RND;
}

bool player_playlist_is_album_mode(play_mode_t mode)
{
    return mode == PLAY_MODE_ALBUM_SEQ || mode == PLAY_MODE_ALBUM_RND;
}

void player_playlist_set_current_group_idx(int group_idx)
{
    s_current_group_idx = group_idx;
}

int player_playlist_get_current_group_idx()
{
    return s_current_group_idx;
}

const std::vector<PlaylistGroup>& player_playlist_artist_groups()
{
    return player_artist_groups_internal();
}

const std::vector<PlaylistGroup>& player_playlist_album_groups()
{
    return player_album_groups_internal();
}

bool player_playlist_align_group_context_for_track(int track_idx, bool verbose)
{
    if (track_idx < 0 || !storage_catalog_v3_ready()) {
        return false;
    }

    rebuild_group_cache_if_needed();

    if (player_playlist_is_artist_mode(g_play_mode)) {
        if (track_idx >= (int)s_group_cache_size || !s_artist_group_index_by_track) return false;
        const int actual_group = (int)s_artist_group_index_by_track[(size_t)track_idx];
        if (actual_group >= 0 && actual_group != s_current_group_idx) {
            if (verbose) {
                LOGD("[播放器] align 歌手 分组: 歌曲=%d old=%d new=%d",
                     track_idx, s_current_group_idx, actual_group);
            }
            s_current_group_idx = actual_group;
            return true;
        }
        return false;
    }

    if (player_playlist_is_album_mode(g_play_mode)) {
        if (track_idx >= (int)s_group_cache_size || !s_album_group_index_by_track) return false;
        const int actual_group = (int)s_album_group_index_by_track[(size_t)track_idx];
        if (actual_group >= 0 && actual_group != s_current_group_idx) {
            if (verbose) {
                LOGD("[播放器] align 专辑 分组: 歌曲=%d old=%d new=%d",
                     track_idx, s_current_group_idx, actual_group);
            }
            s_current_group_idx = actual_group;
            return true;
        }
        return false;
    }

    return false;
}

void player_playlist_update_for_current_track(int current_track_idx, bool verbose)
{
    update_playlist_cache_for_track(current_track_idx);

    s_current_playlist_pos = find_pos_in_playlist(current_track_idx);

    if ((player_playlist_is_artist_mode(g_play_mode) || player_playlist_is_album_mode(g_play_mode)) &&
        s_current_playlist_pos < 0 &&
        player_playlist_align_group_context_for_track(current_track_idx, verbose)) {
        update_playlist_cache_for_track(current_track_idx);
        s_current_playlist_pos = find_pos_in_playlist(current_track_idx);
    }

    s_original_group_pos = -1;
    if (player_playlist_is_artist_mode(g_play_mode)) {
        rebuild_group_cache_if_needed();
        if (current_track_idx >= 0 && 
            current_track_idx < (int)s_group_cache_size && 
            s_artist_group_pos_by_track) {
            s_original_group_pos = (int)s_artist_group_pos_by_track[(size_t)current_track_idx];
        }
    } else if (player_playlist_is_album_mode(g_play_mode)) {
        rebuild_group_cache_if_needed();
        if (current_track_idx >= 0 && 
            current_track_idx < (int)s_group_cache_size && 
            s_album_group_pos_by_track) {
            s_original_group_pos = (int)s_album_group_pos_by_track[(size_t)current_track_idx];
        }
    }
}

void player_playlist_ensure_current()
{
    update_playlist_cache_for_track(-1);
}

size_t player_playlist_current_size()
{
    player_playlist_ensure_current();
    return s_current_playlist_count;
}

bool player_playlist_current_empty()
{
    player_playlist_ensure_current();
    return current_playlist_empty();
}

int player_playlist_current_track_at(size_t pos)
{
    player_playlist_ensure_current();
    return current_playlist_track_at(pos);
}

bool player_playlist_resolve_step(int current_track_idx,
                                  int step,
                                  int& out_track,
                                  bool* out_anchored)
{
    if (out_anchored) *out_anchored = false;

    player_playlist_ensure_current();
    if (current_playlist_empty()) return false;

    int pos = s_current_playlist_pos;
    if (!(pos >= 0 && pos < (int)s_current_playlist_count && current_playlist_track_at((size_t)pos) == current_track_idx)) {
        pos = find_pos_in_playlist(current_track_idx);
    }

    if (pos < 0 || pos >= (int)s_current_playlist_count) {
        if (out_anchored) *out_anchored = true;
        pos = (step >= 0) ? 0 : ((int)s_current_playlist_count - 1);
        out_track = current_playlist_track_at((size_t)pos);
        return true;
    }

    pos += step;
    const int n = (int)s_current_playlist_count;
    while (pos < 0) pos += n;
    while (pos >= n) pos -= n;

    out_track = current_playlist_track_at((size_t)pos);
    return true;
}

bool player_playlist_get_next_for_cover_prefetch(int current_idx,
                                                 int& out_track_idx,
                                                 TrackInfo& out_track)
{
    out_track_idx = -1;
    player_playlist_ensure_current();
    if (current_playlist_empty()) return false;

    int pos = -1;
    if (current_idx >= 0 && current_idx < (int)s_playlist_pos_size && s_playlist_pos_by_track) {
        pos = (int)s_playlist_pos_by_track[(size_t)current_idx];
    }
    if (pos < 0) return false;

    int next_pos = pos + 1;
    if (next_pos >= (int)s_current_playlist_count) next_pos = 0;
    if (next_pos < 0 || next_pos >= (int)s_current_playlist_count) return false;

    const int next_idx = current_playlist_track_at((size_t)next_pos);
    if (next_idx < 0 || next_idx == current_idx) return false;
    if (!storage_catalog_v3_get_trackinfo(next_idx, out_track, "/Music")) return false;

    out_track_idx = next_idx;
    return true;
}

PlayerPlaylistDisplayInfo player_playlist_get_display_info(int current_track_idx,
                                                           int library_total_hint)
{
    PlayerPlaylistDisplayInfo info{};
    info.display_pos = current_track_idx;
    info.display_total = (library_total_hint > 0)
                           ? library_total_hint
                           : (int)storage_catalog_v3_track_count();

    if (player_playlist_is_artist_mode(g_play_mode) || player_playlist_is_album_mode(g_play_mode)) {
        player_playlist_ensure_current();
        info.display_total = (int)s_current_playlist_count;

        if (g_play_mode == PLAY_MODE_ARTIST_RND || g_play_mode == PLAY_MODE_ALBUM_RND) {
            info.display_pos = (s_original_group_pos >= 0) ? s_original_group_pos : s_current_playlist_pos;
        } else {
            info.display_pos = s_current_playlist_pos;
        }
    }

    return info;
}
