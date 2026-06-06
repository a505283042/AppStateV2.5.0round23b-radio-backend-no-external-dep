#include "player_control.h"

#include "audio/audio.h"
#include "audio/audio_service.h"
#include "app_flags.h"
#include "keys/keys.h"
#include "player_playlist.h"
#include "player_assets.h"
#include "radio/radio_catalog.h"
#include "net_music/net_music_catalog.h"
#include "player_state.h"
#include "player_source.h"
#include "lyrics/lyrics.h"
#include "audio/audio_radio_backend.h"
#include "storage/storage.h"
#include "storage/storage_catalog_v3.h"
#include "ui/ui.h"
#include "utils/log.h"

namespace {

PlayerControlHooks s_hooks{};

bool s_user_paused = false;
bool s_manual_stop_latched = false;
uint32_t s_pause_time_ms = 0;

struct RadioReturnContext {
    bool valid = false;
    int track_idx = -1;
    play_mode_t mode = PLAY_MODE_ALL_SEQ;
    int group_idx = -1;
    uint8_t volume = 0;
};

static RadioReturnContext s_radio_return;

static void player_save_radio_return_context_if_needed() {
    const PlayerSourceState source = player_source_get();
    const int cur = player_state_current_index();

    // 只在"从本地歌曲切进电台"时保存
    // 如果本来已经在电台里切台，不覆盖
    if (source.type == PlayerSourceType::NET_RADIO) {
        return;
    }
    if (cur < 0) {
        return;
    }

    s_radio_return.valid = true;
    s_radio_return.track_idx = cur;
    s_radio_return.mode = g_play_mode;
    s_radio_return.group_idx = player_playlist_get_current_group_idx();
    s_radio_return.volume = audio_get_volume();

    LOGI("[RADIO] save return ctx track=%d mode=%d group=%d vol=%u",
         cur,
         (int)s_radio_return.mode,
         s_radio_return.group_idx,
         (unsigned)s_radio_return.volume);
}

int control_current_track_idx()
{
    if (s_hooks.get_current_track_idx) return s_hooks.get_current_track_idx();
    return -1;
}

int control_track_count()
{
    if (s_hooks.get_track_count) return s_hooks.get_track_count();
    return 0;
}

bool control_play_track_dispatch(int idx, bool verbose, bool force_cover)
{
    if (idx < 0) return false;
    if (s_hooks.play_track_dispatch) {
        return s_hooks.play_track_dispatch(idx, verbose, force_cover);
    }
    return false;
}

bool control_enter_list_select_dispatch()
{
    if (s_hooks.enter_list_select) {
        return s_hooks.enter_list_select();
    }
    return false;
}

void control_update_track_pos_for_mode(int current_idx)
{
    if (g_play_mode == PLAY_MODE_ARTIST_SEQ || g_play_mode == PLAY_MODE_ARTIST_RND ||
        g_play_mode == PLAY_MODE_ALBUM_SEQ || g_play_mode == PLAY_MODE_ALBUM_RND) {
        const PlayerPlaylistDisplayInfo display =
            player_playlist_get_display_info(current_idx, (int)storage_catalog_v3_track_count());
        ui_set_track_pos(display.display_pos, display.display_total);
    } else {
        ui_set_track_pos(current_idx, (int)storage_catalog_v3_track_count());
    }
}

void control_prepare_for_radio_source()
{
    player_assets_cancel_pending_cover_prefetch();
    player_assets_invalidate_requests();
    g_lyricsDisplay.clear();
    ui_cover_cache_invalidate();
    ui_set_rotate_wait_prefetch(false);
}

static bool control_is_remote_logo(const String& s)
{
    return s.startsWith("http://") || s.startsWith("https://");
}

static bool control_apply_cover_file(const String& path)
{
    if (path.isEmpty()) return false;

    uint8_t* buf = nullptr;
    size_t len = 0;
    bool is_png = false;

    const bool ok = audio_service_fetch_cover(COVER_FILE_FALLBACK,
                                              "",
                                              path.c_str(),
                                              0,
                                              0,
                                              &buf,
                                              &len,
                                              &is_png,
                                              true);
    if (!ok || !buf || len == 0) {
        if (buf) free(buf);
        return false;
    }

    const bool scaled_ok = ui_cover_scale_from_buffer(buf, len, is_png);
    free(buf);

    if (scaled_ok) {
        ui_request_refresh_now();
    }
    return scaled_ok;
}

static void control_apply_radio_cover(const RadioItem& item)
{
    String logo = item.logo;
    logo.trim();

    if (logo.length() > 0 && !control_is_remote_logo(logo)) {
        if (control_apply_cover_file(logo)) {
            LOGI("[RADIO] logo applied: %s", logo.c_str());
            return;
        }
        LOGW("[RADIO] logo load failed: %s", logo.c_str());
    }

    (void)control_apply_cover_file("/System/default_cover.jpg");
}

static bool control_prepare_net_track_item(int idx, NetMusicItem& item, String& url)
{
    if (idx < 0) return false;

    if (!net_music_catalog_is_loaded() || net_music_catalog_count() == 0) {
        LOGW("[NETTRACK] catalog not loaded or empty");
        return false;
    }

    if (!net_music_catalog_get((uint32_t)idx, &item) || !item.valid) {
        LOGW("[NETTRACK] item not found idx=%d err=%s",
             idx,
             net_music_catalog_error().c_str());
        return false;
    }

    url = net_music_catalog_build_url(item);
    if (!url.length()) {
        LOGW("[NETTRACK] url build failed idx=%d", idx);
        return false;
    }

    return true;
}

static int control_next_net_track_index(int current_idx, int step)
{
    const int count = (int)net_music_catalog_count();
    if (count <= 0) return -1;

    if (current_idx < 0 || current_idx >= count) {
        return step >= 0 ? 0 : count - 1;
    }

    int next = (current_idx + step) % count;
    if (next < 0) next += count;
    return next;
}
} // namespace

void player_control_setup_hooks(const PlayerControlHooks& hooks)
{
    s_hooks = hooks;
}

void player_control_reset_runtime_flags()
{
    s_user_paused = false;
    s_manual_stop_latched = false;
    s_pause_time_ms = 0;
}

void player_control_on_track_started()
{
    player_control_reset_runtime_flags();
}

void player_control_mark_user_paused()
{
    s_user_paused = true;
    s_pause_time_ms = millis();
}

bool player_control_is_user_paused()
{
    return s_user_paused;
}

void player_control_mark_manual_stop()
{
    s_user_paused = true;
    s_manual_stop_latched = true;
}

bool player_control_should_block_idle()
{
    return s_manual_stop_latched && !audio_service_is_playing();
}

bool player_control_try_auto_next(bool entered, bool started)
{
    if (!entered) return false;
    if (s_user_paused) return false;
    if (audio_service_is_playing()) return false;

    const PlayerSourceState source = player_source_get();

    // 网络电台是连续流，不做播完自动下一台。
    if (source.type == PlayerSourceType::NET_RADIO) {
        return false;
    }

    // NAS/HTTP 网络歌曲：文件播放结束后，自动播放下一首。
    // 这里不要依赖 started，因为 NAS 歌曲可能是通过 Web API 直接启动的，
    // player_state.cpp 里的 s_started 不一定会被本地播放流程置 true。
    if (source.type == PlayerSourceType::NET_TRACK) {
        if (!net_music_catalog_is_loaded() || net_music_catalog_count() == 0) {
            LOGW("[NETTRACK] auto next blocked: catalog not loaded or empty");
            return false;
        }

        if (!storage_is_ready() || storage_has_recent_io_error()) {
            LOGW("[NETTRACK] auto next blocked: storage not ready or IO error pending");
            return false;
        }

        const int next = control_next_net_track_index(source.net_track_idx, +1);
        if (next < 0) {
            LOGW("[NETTRACK] auto next failed: invalid next index");
            return false;
        }

        LOGI("[NETTRACK] AUTO NEXT %d -> %d", source.net_track_idx, next);
        return player_play_net_track_index(next);
    }

    // 本地歌曲仍然保留原来的 started 保护，避免刚进播放器时误触发自动播放。
    if (!started) return false;

    const int track_count = control_track_count();
    if (track_count <= 0) return false;

    if (!storage_is_ready() || storage_has_recent_io_error()) {
        LOGW("[PLAYER] auto next blocked: storage not ready or IO error pending");
        return false;
    }

    const int cur = control_current_track_idx();
    int next = 0;
    bool anchored = false;
    if (!player_playlist_resolve_step(cur, +1, next, &anchored)) {
        return false;
    }

    if (anchored) {
        LOGW("[PLAYER] AUTO NEXT anchored to playlist head, mode=%d group=%d cur=%d",
             (int)g_play_mode,
             player_playlist_get_current_group_idx(),
             cur);
    }

    return control_play_track_dispatch(next, false, true);
}


bool player_play_radio_index(int idx)
{
    if (idx < 0) return false;
    const RadioItem* item = radio_catalog_get((size_t)idx);
    if (!item || !item->valid) return false;

    // 保存当前播放状态到返回上下文
    player_save_radio_return_context_if_needed();

    if (player_source_get().type == PlayerSourceType::NET_RADIO) {
        audio_radio_backend_stop();
    }
    if (audio_service_is_playing() || audio_service_is_paused()) {
        audio_service_stop(true);
    }
    control_prepare_for_radio_source();

    player_source_set_radio_stub(idx, *item, String("connecting"), String());
    player_state_set_current_index(-1);
    player_control_reset_runtime_flags();

    ui_set_now_playing(item->name.c_str(), "网络电台");
    ui_set_album(item->region);
    ui_set_track_pos(idx, (int)radio_catalog_count());
    control_apply_radio_cover(*item);
    ui_request_refresh_now();

    const bool ok = audio_radio_backend_start(*item);
    if (ok) {
        player_source_set_radio_status(true, String("connecting"), String());
        player_source_set_radio_runtime(String(audio_radio_backend_name()), String(), 0, String("connecting"), true);
        LOGI("[RADIO] PLAY idx=%d name=%s backend=%s", idx, item->name.c_str(), audio_radio_backend_name());
        return true;
    }

    player_source_set_radio_status(false, String("error"), String("backend_start_failed"));
    LOGW("[RADIO] PLAY failed idx=%d name=%s", idx, item->name.c_str());
    return false;
}

void player_stop_radio()
{
    audio_radio_backend_stop();
    player_source_clear_radio();
}

bool player_return_from_radio_to_local() {
    player_stop_radio();

    if (!s_radio_return.valid || s_radio_return.track_idx < 0) {
        LOGW("[RADIO] no return context");
        return false;
    }

    g_play_mode = s_radio_return.mode;
    player_playlist_set_current_group_idx(s_radio_return.group_idx);
    player_playlist_force_rebuild();

    const bool ok = player_play_idx_v3((uint32_t)s_radio_return.track_idx, true, true);
    if (!ok) {
        LOGW("[RADIO] restore local track failed idx=%d", s_radio_return.track_idx);
        return false;
    }

    audio_set_volume(s_radio_return.volume);
    ui_set_volume(s_radio_return.volume);

    LOGI("[RADIO] restored local track idx=%d", s_radio_return.track_idx);
    return true;
}

bool player_play_net_track_index(int idx)
{
    NetMusicItem item{};
    String url;

    if (!control_prepare_net_track_item(idx, item, url)) {
        return false;
    }

    if (player_source_get().type == PlayerSourceType::NET_RADIO) {
        audio_radio_backend_stop();
    }

    if (audio_service_is_playing() || audio_service_is_paused()) {
        audio_service_stop(true);
    }

    control_prepare_for_radio_source();

    player_source_set_net_track_stub(idx, item, url, String("connecting"), String());
    player_state_set_current_index(-1);
    player_control_reset_runtime_flags();
    audio_service_resume();

    ui_set_now_playing(item.title.c_str(), item.artist.c_str());
    ui_set_album(item.album);
    ui_set_track_pos(idx, (int)net_music_catalog_count());
    ui_set_play_mode(g_play_mode);
    ui_set_volume(audio_get_volume());

    // 第一版先使用默认封面，避免引入网络封面和歌词复杂度。
    (void)control_apply_cover_file("/System/default_cover.jpg");
    ui_request_refresh_now();

    const bool ok = audio_service_play_stream_mp3(url.c_str(), true);
    if (ok) {
        player_source_set_net_track_status(true, String("playing"), String());
        LOGI("[NETTRACK] PLAY idx=%d title=%s url=%s",
             idx,
             item.title.c_str(),
             url.c_str());
        return true;
    }

    player_source_set_net_track_status(false, String("error"), String("stream_start_failed"));
    LOGW("[NETTRACK] PLAY failed idx=%d title=%s url=%s",
         idx,
         item.title.c_str(),
         url.c_str());
    return false;
}

void player_stop_net_track()
{
    audio_service_stop(true);
    player_source_clear_net_track();
}

void player_next_track()
{
    const PlayerSourceState source = player_source_get();
    if (source.type == PlayerSourceType::NET_RADIO) {
        const int count = (int)radio_catalog_count();
        if (count <= 0) return;

        int next_radio = source.radio_idx >= 0 ? (source.radio_idx + 1) % count : 0;

        ui_notify_cover_panel_nav_feedback(1);

        const bool ok = player_play_radio_index(next_radio);
        if (!ok) {
            ui_notify_cover_panel_nav_feedback(0);
        }

        return;
    }

    if (source.type == PlayerSourceType::NET_TRACK) {
        const int next = control_next_net_track_index(source.net_track_idx, +1);
        if (next < 0) return;

        ui_notify_cover_panel_nav_feedback(1);

        const bool ok = player_play_net_track_index(next);
        if (!ok) {
            ui_notify_cover_panel_nav_feedback(0);
        }

        return;
    }

    const int total = control_track_count();
    if (total <= 0) return;

    const int cur = control_current_track_idx();
    int next = 0;
    bool anchored = false;
    if (!player_playlist_resolve_step(cur, +1, next, &anchored)) {
        return;
    }

    if (anchored) {
        LOGW("[PLAYER] NEXT anchored to playlist head, mode=%d group=%d cur=%d",
             (int)g_play_mode, player_playlist_get_current_group_idx(), cur);
    }

    LOGI("[PLAYER] NEXT -> #%d", next);

    ui_notify_cover_panel_nav_feedback(1);

    const bool ok = control_play_track_dispatch(next, false, true);
    if (!ok) {
        ui_notify_cover_panel_nav_feedback(0);
    }
}

void player_prev_track()
{
    const PlayerSourceState source = player_source_get();
    if (source.type == PlayerSourceType::NET_RADIO) {
        const int count = (int)radio_catalog_count();
        if (count <= 0) return;

        int prev_radio = source.radio_idx >= 0 ? (source.radio_idx - 1 + count) % count : 0;

        ui_notify_cover_panel_nav_feedback(-1);

        const bool ok = player_play_radio_index(prev_radio);
        if (!ok) {
            ui_notify_cover_panel_nav_feedback(0);
        }

        return;
    }

    if (source.type == PlayerSourceType::NET_TRACK) {
        const int prev = control_next_net_track_index(source.net_track_idx, -1);
        if (prev < 0) return;

        ui_notify_cover_panel_nav_feedback(-1);

        const bool ok = player_play_net_track_index(prev);
        if (!ok) {
            ui_notify_cover_panel_nav_feedback(0);
        }

        return;
    }

    const int total = control_track_count();
    if (total <= 0) return;

    const int cur = control_current_track_idx();
    int prev = 0;
    bool anchored = false;
    if (!player_playlist_resolve_step(cur, -1, prev, &anchored)) {
        return;
    }

    if (anchored) {
        LOGW("[PLAYER] PREV anchored to playlist tail, mode=%d group=%d cur=%d",
             (int)g_play_mode, player_playlist_get_current_group_idx(), cur);
    }

    LOGI("[PLAYER] PREV -> #%d", prev);

    ui_notify_cover_panel_nav_feedback(-1);

    const bool ok = control_play_track_dispatch(prev, false, true);
    if (!ok) {
        ui_notify_cover_panel_nav_feedback(0);
    }
}

void player_toggle_play()
{
    const PlayerSourceState source = player_source_get();
    const int track_count = control_track_count();
    if (track_count <= 0 &&
        source.type != PlayerSourceType::NET_RADIO &&
        source.type != PlayerSourceType::NET_TRACK) {
        return;
    }
    if (g_rescanning) return;

    if (source.type == PlayerSourceType::NET_RADIO) {
        if (audio_radio_backend_toggle_pause()) {
            const bool paused = audio_radio_backend_is_paused();
            player_source_set_radio_status(true, paused ? String("paused") : String("playing"), String());
            LOGI("[RADIO] %s", paused ? "Paused" : "Resumed");
            return;
        }
        if (source.radio_idx >= 0) {
            (void)player_play_radio_index(source.radio_idx);
            return;
        }
    }

    if (audio_service_is_paused()) {
        audio_service_resume();
        s_user_paused = false;
        s_pause_time_ms = 0;

        if (source.type == PlayerSourceType::NET_TRACK) {
            player_source_set_net_track_status(true, String("playing"), String());
        }

        LOGI("[PLAYER] Resumed from pause");
        return;
    }

    if (audio_service_is_playing()) {
        audio_service_pause();
        s_user_paused = true;
        s_pause_time_ms = millis();
        const uint32_t paused_at_ms = audio_get_play_ms();
        LOGI("[PLAYER] Paused at %u ms", paused_at_ms);
        return;
    }

    if (source.type == PlayerSourceType::NET_RADIO && source.radio_idx >= 0) {
        (void)player_play_radio_index(source.radio_idx);
        return;
    }

    if (source.type == PlayerSourceType::NET_TRACK && source.net_track_idx >= 0) {
        (void)player_play_net_track_index(source.net_track_idx);
        return;
    }

    const int cur = control_current_track_idx();
    if (cur >= 0) {
        LOGI("[PLAYER] Restart current track #%d", cur);
        (void)control_play_track_dispatch(cur, false, true);
    }
}

void player_volume_step(int delta)
{
    int v = (int)audio_get_volume() + delta;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    audio_set_volume((uint8_t)v);
    ui_set_volume((uint8_t)v);
    LOGI("[VOL] %d%%", v);
}

void player_next_group()
{
    const PlayerSourceState source = player_source_get();
    if (source.type == PlayerSourceType::NET_RADIO) {
        if (control_enter_list_select_dispatch()) {
            return;
        }
        LOGW("[LIST] 电台播放中，但无法进入电台列表");
        return;
    }

    if (source.type == PlayerSourceType::NET_TRACK) {
        if (control_enter_list_select_dispatch()) {
            return;
        }
        LOGW("[LIST] NAS歌曲播放中，但无法进入NAS歌曲列表");
        return;
    }

    if (g_play_mode == PLAY_MODE_ARTIST_SEQ || g_play_mode == PLAY_MODE_ARTIST_RND ||
        g_play_mode == PLAY_MODE_ALBUM_SEQ || g_play_mode == PLAY_MODE_ALBUM_RND) {
        if (control_enter_list_select_dispatch()) {
            return;
        }
    }

    const int total = control_track_count();
    if (total <= 0) return;
    const int cur = control_current_track_idx();
    int next = 0;
    bool anchored = false;
    if (!player_playlist_resolve_step(cur, +10, next, &anchored)) {
        return;
    }
    LOGI("[PLAYER] 跳10首 -> #%d", next);
    (void)control_play_track_dispatch(next, false, true);
}

bool control_mode_is_random(play_mode_t mode)
{
    return mode == PLAY_MODE_ALL_RND ||
           mode == PLAY_MODE_ARTIST_RND ||
           mode == PLAY_MODE_ALBUM_RND;
}

namespace {

play_mode_t control_make_mode(int category, bool is_random)
{
    switch (category) {
        case 0: return is_random ? PLAY_MODE_ALL_RND    : PLAY_MODE_ALL_SEQ;
        case 1: return is_random ? PLAY_MODE_ARTIST_RND : PLAY_MODE_ARTIST_SEQ;
        case 2: return is_random ? PLAY_MODE_ALBUM_RND  : PLAY_MODE_ALBUM_SEQ;
        default: return is_random ? PLAY_MODE_ALL_RND   : PLAY_MODE_ALL_SEQ;
    }
}

int control_mode_category(play_mode_t mode)
{
    if (player_playlist_is_artist_mode(mode)) return 1;
    if (player_playlist_is_album_mode(mode)) return 2;
    return 0;
}

void control_apply_mode_context(play_mode_t new_mode, int current_idx, bool verbose)
{
    g_play_mode = new_mode;

    if (current_idx >= 0) {
        (void)player_playlist_align_group_context_for_track(current_idx, verbose);
        player_playlist_update_for_current_track(current_idx, verbose);
    } else {
        player_playlist_force_rebuild();
        player_playlist_get_current();
    }

    control_update_track_pos_for_mode(current_idx);
    ui_set_play_mode(g_play_mode);
}

} // namespace

void player_toggle_random()
{
    const int category = control_mode_category(g_play_mode);
    const bool next_random = !control_mode_is_random(g_play_mode);
    const play_mode_t new_mode = control_make_mode(category, next_random);

    control_apply_mode_context(new_mode, control_current_track_idx(), false);

    LOGI("[PLAYER] 小类切换: %s", next_random ? "随机" : "顺序");
}

void player_cycle_mode_category()
{
    const int cur = control_current_track_idx();
    const bool is_random = control_mode_is_random(g_play_mode);
    const int old_category = control_mode_category(g_play_mode);
    const int new_category = (old_category + 1) % 3;
    const play_mode_t new_mode = control_make_mode(new_category, is_random);

    control_apply_mode_context(new_mode, cur, true);

    const char* cat_name = "全部";
    switch (new_category) {
        case 1: cat_name = "歌手"; break;
        case 2: cat_name = "专辑"; break;
        default: break;
    }

    LOGI("[PLAYER] 大类切换: %s (%s)", cat_name, is_random ? "随机" : "顺序");
}
