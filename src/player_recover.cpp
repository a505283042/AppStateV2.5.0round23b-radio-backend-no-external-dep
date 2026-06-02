#include "player_recover.h"

#include <algorithm>
#include <cstring>
#include <esp_heap_caps.h>

#include <map>
#include <string.h>

#include "app_flags.h"
#include "player_playlist.h"
#include "storage/storage_catalog_v3.h"
#include "storage/storage_view_v3.h"
#include "ui/ui.h"
#include "utils/log.h"

/*
 * 重扫恢复模块。
 *
 * 主要职责：
 * - 维护 path -> track_idx 索引
 * - 在重扫前记住当前歌曲路径
 * - 在重扫完成后恢复当前歌曲与 group 上下文
 */

namespace {

struct StringKeyLess {
    bool operator()(const String& a, const String& b) const {
        return strcmp(a.c_str(), b.c_str()) < 0;
    }
};

PlayerRecoverHooks s_hooks{};
using RecoverTrackIndex = uint16_t;
static constexpr RecoverTrackIndex kInvalidRecoverTrackIndex = 0xFFFF;

RecoverTrackIndex* s_track_idx_sorted_by_path = nullptr;
size_t s_track_idx_sorted_count = 0;
uint32_t s_path_index_generation = 0;
String s_rescan_restore_path;

static inline const char* recover_track_path_cstr(const MusicCatalogV3& cat, int track_idx)
{
    if (track_idx < 0 || track_idx >= (int)cat.track_count) return "";
    const TrackRowV3& row = cat.tracks[(size_t)track_idx];
    return pool_str_v3(cat.pool, row.audio_rel_off);
}

static String recover_normalize_music_path(const String& path)
{
    String s = path;
    s.trim();

    if (s.startsWith("/Music/")) {
        return s.substring(7);   // "/Music/" 去掉，保留相对路径
    }
    if (s == "/Music") {
        return "";
    }
    return s;
}

static int recover_find_track_idx_by_rel_path(const String& rel_path)
{
    if (rel_path.isEmpty() || !storage_catalog_v3_ready()) {
        return -1;
    }

    player_recover_rebuild_path_index_if_needed();

    if (!s_track_idx_sorted_by_path || s_track_idx_sorted_count == 0) {
        return -1;
    }

    const MusicCatalogV3& cat = storage_catalog_v3();
    const char* key = rel_path.c_str();
    if (!key || !key[0]) return -1;

    size_t lo = 0;
    size_t hi = s_track_idx_sorted_count;

    while (lo < hi) {
        size_t mid = lo + ((hi - lo) >> 1);
        int track_idx = (int)s_track_idx_sorted_by_path[mid];
        const char* cur = recover_track_path_cstr(cat, track_idx);
        int cmp = std::strcmp(cur ? cur : "", key);

        if (cmp < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    if (lo < s_track_idx_sorted_count) {
        int track_idx = (int)s_track_idx_sorted_by_path[lo];
        const char* cur = recover_track_path_cstr(cat, track_idx);
        if (std::strcmp(cur ? cur : "", key) == 0) {
            return track_idx;
        }
    }

    return -1;
}

static void free_recover_path_index()
{
    if (s_track_idx_sorted_by_path) {
        heap_caps_free(s_track_idx_sorted_by_path);
        s_track_idx_sorted_by_path = nullptr;
    }
    s_track_idx_sorted_count = 0;
}

static bool ensure_recover_path_index_buf(size_t n)
{
    if (n == 0) {
        free_recover_path_index();
        return true;
    }

    if (s_track_idx_sorted_by_path && s_track_idx_sorted_count == n) {
        return true;
    }

    void* p = heap_caps_realloc(
        s_track_idx_sorted_by_path,
        n * sizeof(RecoverTrackIndex),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (!p) {
        LOGE("[RECOVER] alloc path index failed: count=%u bytes=%u",
             (unsigned)n,
             (unsigned)(n * sizeof(RecoverTrackIndex)));
        return false;
    }

    s_track_idx_sorted_by_path = (RecoverTrackIndex*)p;
    s_track_idx_sorted_count = n;
    return true;
}

int recover_current_track_idx()
{
    if (s_hooks.get_current_track_idx) return s_hooks.get_current_track_idx();
    return -1;
}

bool recover_play_track_dispatch(int idx, bool verbose, bool force_cover)
{
    if (idx < 0) return false;
    if (s_hooks.play_track_dispatch) {
        return s_hooks.play_track_dispatch(idx, verbose, force_cover);
    }
    return false;
}

int recover_clamp_idx_for_dispatch(int idx)
{
    const int total = (int)storage_catalog_v3_track_count();
    if (total <= 0) return -1;
    if (idx < 0) return 0;
    if (idx >= total) return total - 1;
    return idx;
}

} // namespace

void player_recover_setup_hooks(const PlayerRecoverHooks& hooks)
{
    s_hooks = hooks;
}

/* generation 未变化时直接复用；变化后重建 path 索引。 */
void player_recover_rebuild_path_index_if_needed()
{
    if (!storage_catalog_v3_ready()) {
        free_recover_path_index();
        s_path_index_generation = 0;
        return;
    }

    const MusicCatalogV3& cat = storage_catalog_v3();
    const size_t n = (size_t)cat.track_count;

    if (n > 65535) {
        LOGE("[RECOVER] track count too large for uint16 path index: %u", (unsigned)n);
        free_recover_path_index();
        s_path_index_generation = 0;
        return;
    }

    if (s_track_idx_sorted_by_path &&
        s_track_idx_sorted_count == n &&
        s_path_index_generation == cat.generation) {
        return;
    }

    if (!ensure_recover_path_index_buf(n)) {
        free_recover_path_index();
        s_path_index_generation = 0;
        return;
    }

    for (size_t i = 0; i < n; ++i) {
        s_track_idx_sorted_by_path[i] = (RecoverTrackIndex)i;
    }

    std::sort(
        s_track_idx_sorted_by_path,
        s_track_idx_sorted_by_path + n,
        [&cat](RecoverTrackIndex a, RecoverTrackIndex b) {
            const char* pa = recover_track_path_cstr(cat, (int)a);
            const char* pb = recover_track_path_cstr(cat, (int)b);
            return std::strcmp(pa ? pa : "", pb ? pb : "") < 0;
        }
    );

    s_path_index_generation = cat.generation;

    LOGI("[RECOVER] path index tracks=%u bytes=%u (psram)",
         (unsigned)n,
         (unsigned)(n * sizeof(RecoverTrackIndex)));
}

int player_recover_find_track_idx_by_path(const String& path)
{
    if (path.isEmpty() || !storage_catalog_v3_ready()) {
        return -1;
    }

    // 先按原样查一次，兼容本来就存相对路径的场景
    int idx = recover_find_track_idx_by_rel_path(path);
    if (idx >= 0) {
        return idx;
    }

    // 再按 "/Music/xxx" -> "xxx" 归一化后查一次，兼容 NFC / 快照里存完整路径
    String normalized = recover_normalize_music_path(path);
    if (normalized != path) {
        idx = recover_find_track_idx_by_rel_path(normalized);
        if (idx >= 0) {
            LOGI("[RECOVER] path normalized match: %s -> %s -> idx=%d",
                 path.c_str(), normalized.c_str(), idx);
            return idx;
        }
    }

    LOGI("[RECOVER] path lookup miss: %s", path.c_str());
    return -1;
}

bool player_recover_get_current_track_path(String& out_path)
{
    if (!storage_catalog_v3_ready()) {
        out_path = "";
        return false;
    }

    const int cur = recover_current_track_idx();
    if (cur < 0) {
        out_path = "";
        return false;
    }

    TrackViewV3 view;
    if (!storage_catalog_v3_get_track_view((uint32_t)cur, view, "/Music")) {
        out_path = "";
        return false;
    }

    out_path = view.audio_path;
    return !out_path.isEmpty();
}

void player_recover_prepare_rescan_restore_current()
{
    String path;
    if (player_recover_get_current_track_path(path)) {
        s_rescan_restore_path = path;
        LOGD("[PLAYER] rescan restore path saved: %s", s_rescan_restore_path.c_str());
    } else {
        s_rescan_restore_path = "";
        LOGD("[PLAYER] rescan restore path unavailable");
    }
}

/*
 * 处理重扫完成事件：
 * - 成功：切回播放器并恢复原曲
 * - abort/fail：保留旧 catalog，并尽量恢复原曲
 */
bool player_recover_try_handle_rescan_done()
{
    if (!g_rescan_done) return false;

    g_rescan_done = false;

    const int tl_count = (int)storage_catalog_v3_track_count();
    const bool rescan_success = g_rescan_success;
    g_rescanning = false;
    g_rescan_success = false;

    player_recover_rebuild_path_index_if_needed();

    int start_idx = -1;
    if (!s_rescan_restore_path.isEmpty()) {
        start_idx = player_recover_find_track_idx_by_path(s_rescan_restore_path);
        LOGD("[PLAYER] rescan restore lookup: %s -> %d",
             s_rescan_restore_path.c_str(), start_idx);
    }
    if (start_idx < 0) {
        start_idx = recover_clamp_idx_for_dispatch(recover_current_track_idx());
    }
    if (start_idx < 0 && tl_count > 0) {
        start_idx = recover_clamp_idx_for_dispatch(0);
    }

    s_rescan_restore_path = "";

    if (rescan_success && tl_count > 0) {
        ui_enter_player();
        if (start_idx >= 0) {
            (void)player_playlist_align_group_context_for_track(start_idx, true);
            player_playlist_force_rebuild();
            player_playlist_update_for_current_track(start_idx, true);
            (void)recover_play_track_dispatch(start_idx, true, true);
        }
    } else {
        LOGI("[PLAYER] rescan aborted/failed, keep current catalog");
        ui_return_to_player();
        if (start_idx >= 0) {
            LOGI("[PLAYER] restore previous track after rescan abort/fail: idx=%d", start_idx);
            (void)player_playlist_align_group_context_for_track(start_idx, true);
            player_playlist_force_rebuild();
            player_playlist_update_for_current_track(start_idx, true);
            (void)recover_play_track_dispatch(start_idx, true, true);
        }
    }
    return true;
}
