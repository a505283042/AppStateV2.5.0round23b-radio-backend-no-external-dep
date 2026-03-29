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

namespace {

int s_last_playlist_track_total = -1;
bool s_last_playlist_use_v3 = false;
int s_current_group_idx = 0;
std::vector<uint16_t> s_current_playlist;
int s_current_playlist_pos = -1;
int s_original_group_pos = -1;
play_mode_t s_last_play_mode = PLAY_MODE_ALL_SEQ;
int s_last_group_idx = -1;

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
        LOGE("[PLAYER] alloc %s failed: count=%u bytes=%u",
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
    LOGD("[PLAYER] group cache rebuilt: tracks=%lu artists=%d albums=%d gen=%lu",
         (unsigned long)cat.track_count,
         (int)artist_groups.size(),
         (int)album_groups.size(),
         (unsigned long)s_group_cache_generation);
    
    LOGI("[PLAYER][CACHE] group_cache tracks=%u bytes=%u (psram)",
         (unsigned)n,
         (unsigned)(n * 4 * sizeof(CompactIndex)));
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

    for (int pos = 0; pos < (int)s_current_playlist.size(); ++pos) {
        int track_idx = s_current_playlist[pos];
        if (track_idx >= 0 && track_idx < (int)n) {
            s_playlist_pos_by_track[(size_t)track_idx] = (CompactIndex)pos;
        }
    }

    s_playlist_pos_generation = storage_catalog_v3_ready() ? cat.generation : 0;
    s_playlist_pos_mode = g_play_mode;
    s_playlist_pos_group_idx = s_current_group_idx;
    
    LOGI("[PLAYER][CACHE] playlist_pos tracks=%u bytes=%u (psram)",
         (unsigned)n,
         (unsigned)(n * sizeof(CompactIndex)));
}

void shuffle_playlist_keep_current_front(std::vector<uint16_t>& playlist, int current_track)
{
    if (playlist.empty()) return;

    player_playlist_seed_rng_once();

    auto it = std::find(playlist.begin(), playlist.end(), current_track);
    if (it == playlist.end()) {
        std::shuffle(playlist.begin(), playlist.end(), g_rng);
        return;
    }

    std::iter_swap(playlist.begin(), it);
    if (playlist.size() > 1) {
        std::shuffle(playlist.begin() + 1, playlist.end(), g_rng);
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
        s_current_playlist.clear();
        s_current_playlist_pos = -1;

        switch (g_play_mode) {
            case PLAY_MODE_ALL_SEQ:
            case PLAY_MODE_ALL_RND:
                for (int i = 0; i < total; ++i) {
                    s_current_playlist.push_back(i);
                }
                break;

            case PLAY_MODE_ARTIST_SEQ:
            case PLAY_MODE_ARTIST_RND: {
                const auto& groups = player_artist_groups_internal();
                if (s_current_group_idx < 0 || s_current_group_idx >= (int)groups.size()) {
                    s_current_group_idx = 0;
                }
                if (!groups.empty()) {
                    s_current_playlist = groups[s_current_group_idx].track_indices;
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
                    s_current_playlist = groups[s_current_group_idx].track_indices;
                }
                break;
            }
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
        } else if (!s_current_playlist.empty() && current_track_idx >= 0 && current_track_idx < total) {
            s_original_group_pos = current_track_idx;
        }

        const bool is_rnd = (g_play_mode == PLAY_MODE_ALL_RND ||
                             g_play_mode == PLAY_MODE_ARTIST_RND ||
                             g_play_mode == PLAY_MODE_ALBUM_RND);
        if (is_rnd && !s_current_playlist.empty()) {
            shuffle_playlist_keep_current_front(s_current_playlist, current_track_idx);
        }

        rebuild_playlist_pos_cache();
        s_current_playlist_pos = find_pos_in_playlist(current_track_idx);

        if ((player_playlist_is_artist_mode(g_play_mode) || player_playlist_is_album_mode(g_play_mode)) &&
            !s_current_playlist.empty() && s_current_playlist_pos < 0) {
            s_current_playlist_pos = 0;
        }

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
    LOGD("[PLAYER] rng seeded: 0x%08lx", (unsigned long)seed);
}

void player_playlist_reset_state()
{
    s_last_playlist_track_total = -1;
    s_last_playlist_use_v3 = false;
    s_current_group_idx = 0;
    s_current_playlist.clear();
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
                LOGD("[PLAYER] align artist group: track=%d old=%d new=%d",
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
                LOGD("[PLAYER] align album group: track=%d old=%d new=%d",
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

const std::vector<uint16_t>& player_playlist_get_current()
{
    update_playlist_cache_for_track(-1);
    return s_current_playlist;
}

bool player_playlist_resolve_step(int current_track_idx,
                                  int step,
                                  int& out_track,
                                  bool* out_anchored)
{
    if (out_anchored) *out_anchored = false;

    const std::vector<uint16_t>& playlist = player_playlist_get_current();
    if (playlist.empty()) return false;

    int pos = s_current_playlist_pos;
    if (!(pos >= 0 && pos < (int)playlist.size() && playlist[pos] == current_track_idx)) {
        pos = find_pos_in_playlist(current_track_idx);
    }

    if (pos < 0 || pos >= (int)playlist.size()) {
        if (out_anchored) *out_anchored = true;
        pos = (step >= 0) ? 0 : ((int)playlist.size() - 1);
        out_track = playlist[pos];
        return true;
    }

    pos += step;
    const int n = (int)playlist.size();
    while (pos < 0) pos += n;
    while (pos >= n) pos -= n;

    out_track = playlist[pos];
    return true;
}

bool player_playlist_get_next_for_cover_prefetch(int current_idx,
                                                 int& out_track_idx,
                                                 TrackInfo& out_track)
{
    out_track_idx = -1;
    const std::vector<uint16_t>& playlist = player_playlist_get_current();
    if (playlist.empty()) return false;

    int pos = -1;
    if (current_idx >= 0 && current_idx < (int)s_playlist_pos_size && s_playlist_pos_by_track) {
        pos = (int)s_playlist_pos_by_track[(size_t)current_idx];
    }
    if (pos < 0) return false;

    int next_pos = pos + 1;
    if (next_pos >= (int)playlist.size()) next_pos = 0;
    if (next_pos < 0 || next_pos >= (int)playlist.size()) return false;

    const int next_idx = playlist[(size_t)next_pos];
    if (next_idx < 0 || next_idx == current_idx) return false;
    if (!storage_catalog_v3_get_legacy_trackinfo(next_idx, out_track, "/Music")) return false;

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
        const std::vector<uint16_t>& playlist = player_playlist_get_current();
        info.display_total = (int)playlist.size();

        if (g_play_mode == PLAY_MODE_ARTIST_RND || g_play_mode == PLAY_MODE_ALBUM_RND) {
            info.display_pos = (s_original_group_pos >= 0) ? s_original_group_pos : s_current_playlist_pos;
        } else {
            info.display_pos = s_current_playlist_pos;
        }
    }

    return info;
}
